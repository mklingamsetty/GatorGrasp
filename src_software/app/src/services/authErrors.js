export function authErrorInfo(e) {
  switch (e?.code) {
    case 'auth/invalid-email':
      return { title: 'Invalid email', message: "That doesn't look like a valid email address." };
    case 'auth/user-not-found':
      return { title: 'Account not found', message: 'No account exists for that email. Check the spelling or tap Register to create one.' };
    case 'auth/wrong-password':
      return { title: 'Incorrect password', message: 'The password you entered is wrong. Please try again.' };
    case 'auth/invalid-credential':
    case 'auth/invalid-login-credentials':
      return { title: 'Login failed', message: 'Incorrect email or password, or no account exists for that email.' };
    case 'auth/user-disabled':
      return { title: 'Account disabled', message: 'This account has been disabled.' };
    case 'auth/too-many-requests':
      return { title: 'Too many attempts', message: 'Please wait a few minutes before trying again.' };
    case 'auth/network-request-failed':
      return { title: 'No connection', message: 'Check your internet connection and try again.' };
    case 'auth/email-already-in-use':
      return { title: 'Email already registered', message: 'An account with that email already exists. Try logging in instead.' };
    case 'auth/weak-password':
      return { title: 'Weak password', message: 'Password must be at least 6 characters.' };
    default:
      return { title: 'Something went wrong', message: e?.message || 'Unknown error.' };
  }
}
