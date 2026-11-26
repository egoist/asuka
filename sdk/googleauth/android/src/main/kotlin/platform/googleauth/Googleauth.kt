// platform/googleauth on Android: Credential Manager with Google's Sign in
// with Google option (GetSignInWithGoogleOption), the flow Google documents
// for an explicit "Sign in with Google" button: the system's sheet lists
// the device's Google accounts (and offers to add one), the ID token is for
// the server client (the web client's ID). On Android 14 and later the
// sheet is the system's Credential Manager; below, Google Play services'
// (credentials-play-services-auth), back to Android 7.
//
// Scopes besides the profile's go through Google Play services'
// Authorization API for the account chosen: Google's consent screen, when
// the user hasn't granted them yet, launched through the activity's
// result registry; its access token is the credential's.
package platform.googleauth

import android.accounts.Account
import android.app.Activity
import androidx.activity.result.IntentSenderRequest
import androidx.activity.result.contract.ActivityResultContracts
import androidx.credentials.ClearCredentialStateRequest
import androidx.credentials.CredentialManager
import androidx.credentials.CustomCredential
import androidx.credentials.GetCredentialRequest
import androidx.credentials.exceptions.GetCredentialCancellationException
import androidx.credentials.exceptions.GetCredentialException
import androidx.credentials.exceptions.NoCredentialException
import com.google.android.gms.auth.api.identity.AuthorizationRequest
import com.google.android.gms.auth.api.identity.AuthorizationResult
import com.google.android.gms.auth.api.identity.Identity
import com.google.android.gms.common.api.ApiException
import com.google.android.gms.common.api.CommonStatusCodes
import com.google.android.gms.common.api.Scope
import com.google.android.libraries.identity.googleid.GetSignInWithGoogleOption
import com.google.android.libraries.identity.googleid.GoogleIdTokenCredential
import com.google.android.libraries.identity.googleid.GoogleIdTokenParsingException
import dev.asuka.AsukaModules
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.tasks.await
import kotlinx.coroutines.withContext
import org.json.JSONObject

private class GoogleSignIn : Android {
    override suspend fun signIn(serverClientId: String, nonce: String, scopes: List<String>): String {
        val act = withContext(Dispatchers.Main) { AsukaModules.activity }
            ?: throw IllegalStateException("googleauth: no screen to show the account chooser on")
        val option = GetSignInWithGoogleOption.Builder(serverClientId)
            .apply { if (nonce.isNotEmpty()) setNonce(nonce) }
            .build()
        val request = GetCredentialRequest.Builder().addCredentialOption(option).build()
        val got = try {
            withContext(Dispatchers.Main) { CredentialManager.create(act).getCredential(act, request) }
        } catch (e: GetCredentialCancellationException) {
            return ""
        } catch (e: NoCredentialException) {
            throw IllegalStateException("googleauth: no Google account on this device")
        } catch (e: GetCredentialException) {
            throw IllegalStateException("googleauth: ${e.errorMessage ?: e.type}")
        }
        val c = got.credential
        if (c !is CustomCredential || (c.type != GoogleIdTokenCredential.TYPE_GOOGLE_ID_TOKEN_CREDENTIAL &&
                c.type != GoogleIdTokenCredential.TYPE_GOOGLE_ID_TOKEN_SIWG_CREDENTIAL)) {
            throw IllegalStateException("googleauth: Credential Manager answered a ${c.type}, not a Google ID token")
        }
        val g = try {
            GoogleIdTokenCredential.createFrom(c.data)
        } catch (e: GoogleIdTokenParsingException) {
            throw IllegalStateException("googleauth: Google's credential can't be read: ${e.message}")
        }
        // The account's stable ID is the token's sub (g.id is its email,
        // which can change); googleid reads it, and the email, from the
        // token's claims.
        val email = g.email.orEmpty().ifEmpty { if (g.id.contains('@')) g.id else "" }
        var accessToken = ""
        if (scopes.isNotEmpty()) {
            accessToken = authorize(act, email, scopes) ?: return ""
        }
        return JSONObject()
            .put("idToken", g.idToken)
            .put("id", g.uniqueId)
            .put("email", email)
            .put("name", g.displayName ?: "")
            .put("givenName", g.givenName ?: "")
            .put("familyName", g.familyName ?: "")
            .put("picture", g.profilePictureUri?.toString() ?: "")
            .put("accessToken", accessToken)
            .toString()
    }

    /**
     * The access token for scopes, for the account signed in with:
     * Google's consent first if the user hasn't granted them; none if
     * they declined.
     */
    private suspend fun authorize(act: Activity, email: String, scopes: List<String>): String? {
        val client = Identity.getAuthorizationClient(act)
        val request = AuthorizationRequest.builder()
            .setRequestedScopes(scopes.distinct().map { Scope(it) })
            .apply { if (email.isNotEmpty()) setAccount(Account(email, "com.google")) }
            .build()
        var result: AuthorizationResult = try {
            client.authorize(request).await()
        } catch (e: ApiException) {
            throw IllegalStateException("googleauth: the authorization failed: ${statusOf(e)}")
        }
        if (result.hasResolution()) {
            val intent = result.pendingIntent
                ?: throw IllegalStateException("googleauth: the authorization has no consent screen to show")
            val answer = AsukaModules.launch(ActivityResultContracts.StartIntentSenderForResult(),
                IntentSenderRequest.Builder(intent.intentSender).build())
            val data = answer.data
            if (answer.resultCode != Activity.RESULT_OK || data == null) return null
            result = try {
                client.getAuthorizationResultFromIntent(data)
            } catch (e: ApiException) {
                if (e.statusCode == CommonStatusCodes.CANCELED) return null
                throw IllegalStateException("googleauth: the authorization failed: ${statusOf(e)}")
            }
        }
        return result.accessToken
            ?: throw IllegalStateException("googleauth: the authorization gave no access token")
    }

    override suspend fun signOut() {
        CredentialManager.create(AsukaModules.context).clearCredentialState(ClearCredentialStateRequest())
    }
}

private fun statusOf(e: ApiException): String =
    e.status.statusMessage ?: CommonStatusCodes.getStatusCodeString(e.statusCode)

fun makeAndroid(): Android = GoogleSignIn()
