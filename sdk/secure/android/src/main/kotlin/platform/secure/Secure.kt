// platform/secure on Android: each value sealed (AES-GCM, its IV first)
// with a key the Android Keystore keeps and never lets out, then kept,
// base64, in the app's private preferences. The key is the host's own
// ("asuka.secure"), so values its secure store kept are read here.
//
// A protected value is sealed with a fresh AES key, itself sealed with the
// public half of an RSA pair in the Keystore ("asuka.secure.protected")
// whose private half works only once the user proves it's them, for each
// use. Writing needs only the public half, so it asks nothing (as the
// Keychain on iOS); reading unseals the AES key through BiometricPrompt,
// the private key's cipher its CryptoObject. On Android 11 and later the
// private key takes a strong biometric or the screen lock (PIN, pattern,
// password). Below, a key bound to every use can only be unlocked by a
// fingerprint (the screen lock unlocks keys for a time, not per use, and
// BiometricPrompt can't ask for it with a CryptoObject there): so on 7 to
// 10, protected takes an enrolled fingerprint, or throws notAvailable.
// Kept in the same preferences, "p:" before the base64 (never in base64),
// so a key holds one value either way and writing replaces it at once.
package platform.secure

import android.app.KeyguardManager
import android.content.Context
import android.os.Build
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyPermanentlyInvalidatedException
import android.security.keystore.KeyProperties
import android.util.Base64
import androidx.biometric.BiometricManager
import androidx.biometric.BiometricManager.Authenticators.BIOMETRIC_STRONG
import androidx.biometric.BiometricManager.Authenticators.DEVICE_CREDENTIAL
import androidx.biometric.BiometricPrompt
import androidx.core.content.ContextCompat
import androidx.fragment.app.FragmentActivity
import dev.asuka.AsukaModules
import dev.asuka.AsukaUnsupportedException
import java.nio.ByteBuffer
import java.security.KeyFactory
import java.security.KeyPairGenerator
import java.security.KeyStore
import java.security.PrivateKey
import java.security.PublicKey
import java.security.spec.MGF1ParameterSpec
import java.security.spec.X509EncodedKeySpec
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.OAEPParameterSpec
import javax.crypto.spec.PSource
import javax.crypto.spec.SecretKeySpec
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext

private const val ALIAS = "asuka.secure"
private const val PROTECTED_ALIAS = "asuka.secure.protected"
private const val PROTECTED = "p:"
private const val RSA = "RSA/ECB/OAEPWithSHA-256AndMGF1Padding"

// The Keystore's OAEP takes SHA-1 for MGF1 (before Android 12, only it).
private val OAEP = OAEPParameterSpec("SHA-256", "MGF1", MGF1ParameterSpec.SHA1, PSource.PSpecified.DEFAULT)

class KeystoreKeychain(private val app: Context) : Keychain {
    private val prefs = app.getSharedPreferences("asuka.secure", Context.MODE_PRIVATE)

    override suspend fun get(key: String, prompt: String): String? {
        val v = withContext(Dispatchers.IO) { prefs.getString(key, null) } ?: return null
        if (!v.startsWith(PROTECTED)) return withContext(Dispatchers.IO) { open(v) }
        // [wrapped key's length, 2 bytes][wrapped AES key][IV, 12][sealed]
        val raw = ByteBuffer.wrap(Base64.decode(v.substring(PROTECTED.length), Base64.NO_WRAP))
        val wrapped = ByteArray(raw.short.toInt() and 0xffff).also { raw.get(it) }
        val sealed = ByteArray(raw.remaining()).also { raw.get(it) }
        val cipher = withContext(Dispatchers.IO) {
            try {
                Cipher.getInstance(RSA).apply { init(Cipher.DECRYPT_MODE, privateKey(), OAEP) }
            } catch (e: KeyPermanentlyInvalidatedException) {
                // The screen lock was removed (or every fingerprint, below
                // Android 11): the key is gone, and with it the value.
                forgetProtected(key)
                null
            }
        } ?: return null
        if (!canAsk()) throw SecureException(SecureError.NOT_AVAILABLE)
        val unlocked = ask(prompt, cipher)
        return withContext(Dispatchers.IO) {
            val aes = SecretKeySpec(unlocked.doFinal(wrapped), "AES")
            val c = Cipher.getInstance("AES/GCM/NoPadding")
            c.init(Cipher.DECRYPT_MODE, aes, GCMParameterSpec(128, sealed, 0, 12))
            String(c.doFinal(sealed, 12, sealed.size - 12), Charsets.UTF_8)
        }
    }

    override suspend fun set(key: String, value: String, protected: Boolean) = withContext(Dispatchers.IO) {
        val v = if (protected) sealProtected(value) else seal(value)
        if (!prefs.edit().putString(key, v).commit()) {
            throw java.io.IOException("secure: couldn't write the value")
        }
    }

    override suspend fun remove(key: String) = withContext(Dispatchers.IO) {
        prefs.edit().remove(key).commit()
        kotlin.Unit
    }

    private fun seal(value: String): String {
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.ENCRYPT_MODE, secretKey())
        val sealed = c.iv + c.doFinal(value.toByteArray(Charsets.UTF_8))
        return Base64.encodeToString(sealed, Base64.NO_WRAP)
    }

    private fun open(v: String): String {
        val raw = Base64.decode(v, Base64.NO_WRAP)
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.DECRYPT_MODE, secretKey(), GCMParameterSpec(128, raw, 0, 12))
        return String(c.doFinal(raw, 12, raw.size - 12), Charsets.UTF_8)
    }

    /**
     * The device can't keep a protected key: logged with why (the
     * Keystore's own words: "At least one biometric must be enrolled…"),
     * since the app sees only SecureError.notAvailable.
     */
    private fun notAvailable(e: Exception): Nothing {
        android.util.Log.w("asuka", "secure: a protected key can't be made here: $e${e.cause?.let { " ($it)" } ?: ""}")
        throw SecureException(SecureError.NOT_AVAILABLE)
    }

    private fun sealProtected(value: String): String {
        if (!canAsk()) throw SecureException(SecureError.NOT_AVAILABLE)
        val pub = try {
            publicKey()
        } catch (e: IllegalStateException) {
            notAvailable(e) // no screen lock, or nothing enrolled
        } catch (e: java.security.InvalidAlgorithmParameterException) {
            notAvailable(e)
        }
        val aes = KeyGenerator.getInstance("AES").apply { init(256) }.generateKey()
        val c = Cipher.getInstance("AES/GCM/NoPadding")
        c.init(Cipher.ENCRYPT_MODE, aes)
        val sealed = c.iv + c.doFinal(value.toByteArray(Charsets.UTF_8))
        val wrapped = Cipher.getInstance(RSA).apply { init(Cipher.ENCRYPT_MODE, pub, OAEP) }.doFinal(aes.encoded)
        val out = ByteBuffer.allocate(2 + wrapped.size + sealed.size)
        out.putShort(wrapped.size.toShort()).put(wrapped).put(sealed)
        return PROTECTED + Base64.encodeToString(out.array(), Base64.NO_WRAP)
    }

    /** What may unlock the protected key: see the file's comment. */
    private fun allowed(): Int =
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) BIOMETRIC_STRONG or DEVICE_CREDENTIAL else BIOMETRIC_STRONG

    private fun canAsk(): Boolean {
        val keyguard = app.getSystemService(Context.KEYGUARD_SERVICE) as KeyguardManager
        return keyguard.isDeviceSecure &&
            BiometricManager.from(app).canAuthenticate(allowed()) == BiometricManager.BIOMETRIC_SUCCESS
    }

    /** BiometricPrompt over the app's activity, the cipher its CryptoObject: the cipher, unlocked. */
    private suspend fun ask(prompt: String, cipher: Cipher): Cipher = withContext(Dispatchers.Main) {
        val act = AsukaModules.activity as? FragmentActivity ?: throw AsukaUnsupportedException("secure: no activity shows the app")
        val title = prompt.ifEmpty { app.applicationInfo.loadLabel(app.packageManager).toString() }
        val info = BiometricPrompt.PromptInfo.Builder().setTitle(title).setAllowedAuthenticators(allowed())
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.R) info.setNegativeButtonText(act.getString(android.R.string.cancel))
        suspendCancellableCoroutine<Cipher> { cont ->
            val p = BiometricPrompt(act, ContextCompat.getMainExecutor(act), object : BiometricPrompt.AuthenticationCallback() {
                override fun onAuthenticationSucceeded(result: BiometricPrompt.AuthenticationResult) {
                    if (!cont.isActive) return
                    val c = result.cryptoObject?.cipher
                    if (c != null) cont.resume(c) else cont.resumeWithException(SecureException(SecureError.CANCELLED))
                }

                override fun onAuthenticationError(code: Int, message: CharSequence) {
                    if (!cont.isActive) return
                    cont.resumeWithException(SecureException(when (code) {
                        BiometricPrompt.ERROR_NO_BIOMETRICS, BiometricPrompt.ERROR_HW_NOT_PRESENT,
                        BiometricPrompt.ERROR_HW_UNAVAILABLE, BiometricPrompt.ERROR_NO_DEVICE_CREDENTIAL -> SecureError.NOT_AVAILABLE
                        else -> SecureError.CANCELLED // cancelled, the negative button, locked out, timed out
                    }))
                }
                // onAuthenticationFailed: a try that wasn't the owner; the prompt stays for another.
            })
            cont.invokeOnCancellation { act.runOnUiThread { p.cancelAuthentication() } }
            p.authenticate(info.build(), BiometricPrompt.CryptoObject(cipher))
        }
    }

    @Synchronized
    private fun forgetProtected(key: String) {
        prefs.edit().remove(key).commit()
        KeyStore.getInstance("AndroidKeyStore").apply { load(null) }.deleteEntry(PROTECTED_ALIAS)
    }

    /** The key, in the Android Keystore: made the first time. */
    @Synchronized
    private fun secretKey(): SecretKey {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (ks.getKey(ALIAS, null) as? SecretKey)?.let { return it }
        val g = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        g.init(KeyGenParameterSpec.Builder(ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .build())
        return g.generateKey()
    }

    private fun privateKey(): PrivateKey {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        return ks.getKey(PROTECTED_ALIAS, null) as? PrivateKey
            ?: throw KeyPermanentlyInvalidatedException("secure: the protected key is gone")
    }

    /**
     * The protected pair's public half, made the first time, as a key of
     * the platform's own provider (the Keystore's won't encrypt with OAEP's
     * SHA-256 everywhere; the public half needs no protection).
     */
    @Synchronized
    private fun publicKey(): PublicKey {
        val ks = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        val pub = ks.getCertificate(PROTECTED_ALIAS)?.publicKey ?: run {
            val b = KeyGenParameterSpec.Builder(PROTECTED_ALIAS, KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setKeySize(2048)
                .setDigests(KeyProperties.DIGEST_SHA256, KeyProperties.DIGEST_SHA1)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_RSA_OAEP)
                .setUserAuthenticationRequired(true)
                .setInvalidatedByBiometricEnrollment(false)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                b.setUserAuthenticationParameters(0, KeyProperties.AUTH_BIOMETRIC_STRONG or KeyProperties.AUTH_DEVICE_CREDENTIAL)
            } else {
                @Suppress("DEPRECATION")
                b.setUserAuthenticationValidityDurationSeconds(-1)
            }
            val g = KeyPairGenerator.getInstance(KeyProperties.KEY_ALGORITHM_RSA, "AndroidKeyStore")
            g.initialize(b.build())
            g.generateKeyPair().public
        }
        return KeyFactory.getInstance(pub.algorithm).generatePublic(X509EncodedKeySpec(pub.encoded))
    }
}

fun makeKeychain(): Keychain = KeystoreKeychain(AsukaModules.context)
