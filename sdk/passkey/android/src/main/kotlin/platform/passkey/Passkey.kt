// platform/passkey on Android: Credential Manager's passkeys, the system's
// sheet over the activity showing the app (Google Password Manager, or the
// provider the user chose; Google Play services' below Android 14). It
// takes the server's options as WebAuthn's JSON and gives back the
// platform's own response JSON, which Asuka's side completes. The
// relying party's domain must name the app in its assetlinks.json
// (delegate_permission/common.get_login_creds, with the app's signing
// certificate's SHA-256), and its server must take the app's origin,
// android:apk-key-hash:<the certificate's SHA-256, base64url>.
package platform.passkey

import android.os.Build
import androidx.credentials.CreateCredentialResponse
import androidx.credentials.CreatePublicKeyCredentialRequest
import androidx.credentials.CreatePublicKeyCredentialResponse
import androidx.credentials.CredentialManager
import androidx.credentials.GetCredentialRequest
import androidx.credentials.GetPublicKeyCredentialOption
import androidx.credentials.PublicKeyCredential
import androidx.credentials.exceptions.CreateCredentialCancellationException
import androidx.credentials.exceptions.CreateCredentialNoCreateOptionException
import androidx.credentials.exceptions.CreateCredentialProviderConfigurationException
import androidx.credentials.exceptions.CreateCredentialUnsupportedException
import androidx.credentials.exceptions.GetCredentialCancellationException
import androidx.credentials.exceptions.GetCredentialProviderConfigurationException
import androidx.credentials.exceptions.GetCredentialUnsupportedException
import androidx.credentials.exceptions.NoCredentialException
import androidx.credentials.exceptions.domerrors.DomError
import androidx.credentials.exceptions.domerrors.InvalidStateError
import androidx.credentials.exceptions.domerrors.NotAllowedError
import androidx.credentials.exceptions.domerrors.NotSupportedError
import androidx.credentials.exceptions.domerrors.SecurityError
import androidx.credentials.exceptions.publickeycredential.CreatePublicKeyCredentialDomException
import androidx.credentials.exceptions.publickeycredential.GetPublicKeyCredentialDomException
import dev.asuka.AsukaModules
import dev.asuka.AsukaUnsupportedException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

private class CredentialManagerPasskeys : Authenticator {
    // Passkeys need Android 9 (Credential Manager's providers, through
    // Google Play services before 14).
    override suspend fun available(): Boolean = Build.VERSION.SDK_INT >= Build.VERSION_CODES.P

    override suspend fun register(c: Creation): Response = withContext(Dispatchers.Main) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) throw PasskeyException(PasskeyError.NOT_AVAILABLE)
        val act = AsukaModules.activity ?: throw AsukaUnsupportedException("passkey: no screen to show the sheet on")
        val r: CreateCredentialResponse = try {
            CredentialManager.create(act).createCredential(act, CreatePublicKeyCredentialRequest(c.json))
        } catch (e: Exception) {
            throw passkeyError(e)
        }
        val p = r as? CreatePublicKeyCredentialResponse ?: throw IllegalStateException("passkey: Credential Manager answered a ${r.type}")
        Response(json = p.registrationResponseJson)
    }

    override suspend fun signIn(r: Request): Response = withContext(Dispatchers.Main) {
        if (Build.VERSION.SDK_INT < Build.VERSION_CODES.P) throw PasskeyException(PasskeyError.NOT_AVAILABLE)
        val act = AsukaModules.activity ?: throw AsukaUnsupportedException("passkey: no screen to show the sheet on")
        val got = try {
            CredentialManager.create(act).getCredential(act, GetCredentialRequest(listOf(GetPublicKeyCredentialOption(r.json))))
        } catch (e: Exception) {
            throw passkeyError(e)
        }
        val c = got.credential as? PublicKeyCredential ?: throw IllegalStateException("passkey: Credential Manager answered a ${got.credential.type}")
        Response(json = c.authenticationResponseJson)
    }
}

/** Credential Manager's exception as the package's error, where it's one of them. */
internal fun passkeyError(e: Exception): Exception = when (e) {
    is CreateCredentialCancellationException, is GetCredentialCancellationException -> PasskeyException(PasskeyError.CANCELED)
    is NoCredentialException -> PasskeyException(PasskeyError.NO_CREDENTIALS)
    is CreateCredentialNoCreateOptionException, is CreateCredentialProviderConfigurationException, is CreateCredentialUnsupportedException,
    is GetCredentialProviderConfigurationException, is GetCredentialUnsupportedException -> PasskeyException(PasskeyError.NOT_AVAILABLE)
    is CreatePublicKeyCredentialDomException -> domError(e.domError) ?: e
    is GetPublicKeyCredentialDomException -> domError(e.domError) ?: e
    else -> e
}

/** WebAuthn's DOMException, as a browser's maps: an excluded passkey, a domain that isn't the app's, the user's no. */
private fun domError(d: DomError): PasskeyException? = when (d) {
    is InvalidStateError -> PasskeyException(PasskeyError.EXISTS)
    is SecurityError -> PasskeyException(PasskeyError.NOT_ASSOCIATED)
    is NotAllowedError -> PasskeyException(PasskeyError.CANCELED)
    is NotSupportedError -> PasskeyException(PasskeyError.NOT_AVAILABLE)
    else -> null
}

fun makeAuthenticator(): Authenticator = CredentialManagerPasskeys()
