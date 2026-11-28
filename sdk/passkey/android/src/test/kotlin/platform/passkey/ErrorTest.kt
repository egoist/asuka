// platform/passkey's errors on Android, on this computer's JVM: Credential
// Manager's exceptions as the package's errors, as a browser maps
// WebAuthn's DOMExceptions.
package platform.passkey

import androidx.credentials.exceptions.CreateCredentialCancellationException
import androidx.credentials.exceptions.CreateCredentialProviderConfigurationException
import androidx.credentials.exceptions.GetCredentialCancellationException
import androidx.credentials.exceptions.NoCredentialException
import androidx.credentials.exceptions.domerrors.InvalidStateError
import androidx.credentials.exceptions.domerrors.NotAllowedError
import androidx.credentials.exceptions.domerrors.SecurityError
import androidx.credentials.exceptions.domerrors.UnknownError
import androidx.credentials.exceptions.publickeycredential.CreatePublicKeyCredentialDomException
import androidx.credentials.exceptions.publickeycredential.GetPublicKeyCredentialDomException
import org.junit.Assert.assertEquals
import org.junit.Assert.assertSame
import org.junit.Test

class ErrorTest {
    private fun declared(e: Exception): PasskeyError? = (passkeyError(e) as? PasskeyException)?.error

    @Test
    fun theUsersNo() {
        assertEquals(PasskeyError.CANCELED, declared(CreateCredentialCancellationException()))
        assertEquals(PasskeyError.CANCELED, declared(GetCredentialCancellationException()))
        assertEquals(PasskeyError.CANCELED, declared(GetPublicKeyCredentialDomException(NotAllowedError())))
    }

    @Test
    fun whatTheDeviceHas() {
        assertEquals(PasskeyError.NO_CREDENTIALS, declared(NoCredentialException()))
        assertEquals(PasskeyError.EXISTS, declared(CreatePublicKeyCredentialDomException(InvalidStateError())))
        assertEquals(PasskeyError.NOT_ASSOCIATED, declared(GetPublicKeyCredentialDomException(SecurityError())))
        assertEquals(PasskeyError.NOT_AVAILABLE, declared(CreateCredentialProviderConfigurationException()))
    }

    @Test
    fun othersAsTheyAre() {
        val e = CreatePublicKeyCredentialDomException(UnknownError(), "boom")
        assertSame(e, passkeyError(e))
        val other = IllegalStateException("x")
        assertSame(other, passkeyError(other))
    }
}
