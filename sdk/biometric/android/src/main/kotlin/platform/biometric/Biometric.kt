// platform/biometric on Android: BiometricManager and BiometricPrompt, over
// the activity showing the app (a FragmentActivity, the host's).
package platform.biometric

import android.app.KeyguardManager
import android.content.Context
import android.content.pm.PackageManager
import androidx.biometric.BiometricManager
import androidx.biometric.BiometricManager.Authenticators.BIOMETRIC_STRONG
import androidx.biometric.BiometricManager.Authenticators.BIOMETRIC_WEAK
import androidx.biometric.BiometricManager.Authenticators.DEVICE_CREDENTIAL
import androidx.biometric.BiometricPrompt
import androidx.core.content.ContextCompat
import androidx.fragment.app.FragmentActivity
import dev.asuka.AsukaModules
import dev.asuka.AsukaUnsupportedException
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext

class PromptAuthenticator(private val app: Context) : Authenticator {
    override suspend fun check(): Status {
        val pm = app.packageManager
        val kind = when {
            pm.hasSystemFeature(PackageManager.FEATURE_FINGERPRINT) -> Kind.FINGERPRINT
            pm.hasSystemFeature(PackageManager.FEATURE_FACE) -> Kind.FACE
            pm.hasSystemFeature(PackageManager.FEATURE_IRIS) -> Kind.IRIS
            else -> Kind.NONE
        }
        val enrolled = BiometricManager.from(app).canAuthenticate(BIOMETRIC_WEAK) == BiometricManager.BIOMETRIC_SUCCESS
        val passcode = (app.getSystemService(Context.KEYGUARD_SERVICE) as KeyguardManager).isDeviceSecure
        return Status(kind = kind, enrolled = enrolled, passcode = passcode)
    }

    override suspend fun authenticate(reason: String, cancel: String, noPasscode: Boolean) = withContext(Dispatchers.Main) {
        val act = AsukaModules.activity as? FragmentActivity ?: throw AsukaUnsupportedException("biometric: no activity shows the app")
        // A strong biometric alone; or any, or the device's PIN, pattern or
        // password (BIOMETRIC_STRONG with DEVICE_CREDENTIAL isn't on
        // Android 9 and 10).
        val allowed = if (noPasscode) BIOMETRIC_STRONG else BIOMETRIC_WEAK or DEVICE_CREDENTIAL
        if (BiometricManager.from(app).canAuthenticate(allowed) != BiometricManager.BIOMETRIC_SUCCESS) {
            throw BiometricException(BiometricError.NOT_AVAILABLE)
        }
        val title = reason.ifEmpty { app.applicationInfo.loadLabel(app.packageManager).toString() }
        val info = BiometricPrompt.PromptInfo.Builder().setTitle(title).setAllowedAuthenticators(allowed)
        if (noPasscode) info.setNegativeButtonText(cancel.ifEmpty { act.getString(android.R.string.cancel) })
        suspendCancellableCoroutine<kotlin.Unit> { cont ->
            val prompt = BiometricPrompt(act, ContextCompat.getMainExecutor(act), object : BiometricPrompt.AuthenticationCallback() {
                override fun onAuthenticationSucceeded(result: BiometricPrompt.AuthenticationResult) {
                    if (cont.isActive) cont.resume(kotlin.Unit)
                }

                override fun onAuthenticationError(code: Int, message: CharSequence) {
                    if (!cont.isActive) return
                    cont.resumeWithException(when (code) {
                        BiometricPrompt.ERROR_USER_CANCELED, BiometricPrompt.ERROR_NEGATIVE_BUTTON, BiometricPrompt.ERROR_CANCELED ->
                            BiometricException(BiometricError.CANCELED)
                        BiometricPrompt.ERROR_NO_BIOMETRICS, BiometricPrompt.ERROR_HW_NOT_PRESENT, BiometricPrompt.ERROR_HW_UNAVAILABLE,
                        BiometricPrompt.ERROR_LOCKOUT, BiometricPrompt.ERROR_LOCKOUT_PERMANENT, BiometricPrompt.ERROR_NO_DEVICE_CREDENTIAL ->
                            BiometricException(BiometricError.NOT_AVAILABLE)
                        else -> Exception(message.toString())
                    })
                }
                // onAuthenticationFailed: a try that wasn't the owner; the prompt stays for another.
            })
            cont.invokeOnCancellation { act.runOnUiThread { prompt.cancelAuthentication() } }
            prompt.authenticate(info.build())
        }
    }
}

fun makeAuthenticator(): Authenticator = PromptAuthenticator(AsukaModules.context)
