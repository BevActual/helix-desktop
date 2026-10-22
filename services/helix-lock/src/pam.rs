//! Checking a password with PAM. Only `pam_authenticate` and `pam_acct_mgmt` are used (this is an unlock, not a login: no session, no credentials).
//! The password is answered to whatever PAM asks for a secret and to nothing else; informational messages are acknowledged and dropped.

use std::ffi::{c_char, c_int, c_void, CString};

#[repr(C)]
struct Message {
    style: c_int,
    text: *const c_char,
}

#[repr(C)]
struct Response {
    text: *mut c_char,
    code: c_int,
}

type Conversation =
    extern "C" fn(c_int, *mut *const Message, *mut *mut Response, *mut c_void) -> c_int;

#[repr(C)]
struct Conv {
    function: Conversation,
    data: *mut c_void,
}

#[link(name = "pam")]
extern "C" {
    fn pam_start(
        service: *const c_char,
        user: *const c_char,
        conv: *const Conv,
        handle: *mut *mut c_void,
    ) -> c_int;
    fn pam_start_confdir(
        service: *const c_char,
        user: *const c_char,
        conv: *const Conv,
        confdir: *const c_char,
        handle: *mut *mut c_void,
    ) -> c_int;
    fn pam_authenticate(handle: *mut c_void, flags: c_int) -> c_int;
    fn pam_acct_mgmt(handle: *mut c_void, flags: c_int) -> c_int;
    fn pam_end(handle: *mut c_void, status: c_int) -> c_int;
}

const SUCCESS: c_int = 0;
const CONV_ERR: c_int = 19;
const PROMPT_ECHO_OFF: c_int = 1;
const PROMPT_ECHO_ON: c_int = 2;

#[derive(Debug, PartialEq, Eq)]
pub enum Outcome {
    Accepted,
    /// The password was wrong (or the account may not log in): try again.
    Refused,
    /// PAM itself could not run; the lock stays.
    Broken(String),
}

extern "C" fn answer(
    count: c_int,
    messages: *mut *const Message,
    responses: *mut *mut Response,
    data: *mut c_void,
) -> c_int {
    if count <= 0 || messages.is_null() || responses.is_null() || data.is_null() {
        return CONV_ERR;
    }
    // SAFETY: `data` is the `CString` this module passed to `pam_start`, alive for the whole call; PAM gives `count` valid message pointers and takes
    // ownership of the `calloc`ed array of responses and of each `strdup`ed string.
    unsafe {
        let secret = &*(data as *const CString);
        let array = libc::calloc(count as usize, std::mem::size_of::<Response>()) as *mut Response;
        if array.is_null() {
            return CONV_ERR;
        }
        for index in 0..count as isize {
            let message = &**messages.offset(index);
            if matches!(message.style, PROMPT_ECHO_OFF | PROMPT_ECHO_ON) {
                let copy = libc::strdup(secret.as_ptr());
                if copy.is_null() {
                    for earlier in 0..index {
                        let text = (*array.offset(earlier)).text;
                        if !text.is_null() {
                            libc::free(text as *mut c_void);
                        }
                    }
                    libc::free(array as *mut c_void);
                    return CONV_ERR;
                }
                (*array.offset(index)).text = copy;
            }
        }
        *responses = array;
    }
    SUCCESS
}

/// Asks PAM whether `password` is `user`'s. Blocking (PAM may delay a refusal on purpose); call it from a thread.
pub fn authenticate(service: &str, user: &str, password: &str) -> Outcome {
    authenticate_in(None, service, user, password)
}

/// The same, reading the service's configuration from `confdir` instead of `/etc/pam.d` (a test's own stack; nothing else sets it).
pub fn authenticate_in(
    confdir: Option<&str>,
    service: &str,
    user: &str,
    password: &str,
) -> Outcome {
    let (Ok(service), Ok(user), Ok(secret)) = (
        CString::new(service),
        CString::new(user),
        CString::new(password),
    ) else {
        return Outcome::Refused;
    };
    let Ok(confdir) = confdir.map(CString::new).transpose() else {
        return Outcome::Refused;
    };
    let conv = Conv {
        function: answer,
        data: &secret as *const CString as *mut c_void,
    };
    let mut handle: *mut c_void = std::ptr::null_mut();
    // SAFETY: the pointers are to live, NUL-terminated strings and a live `Conv`; `handle` is ended on every path after a successful start.
    unsafe {
        let started = match &confdir {
            Some(dir) => pam_start_confdir(
                service.as_ptr(),
                user.as_ptr(),
                &conv,
                dir.as_ptr(),
                &mut handle,
            ),
            None => pam_start(service.as_ptr(), user.as_ptr(), &conv, &mut handle),
        };
        if started != SUCCESS {
            return Outcome::Broken(format!("pam_start failed ({started})"));
        }
        let mut status = pam_authenticate(handle, 0);
        if status == SUCCESS {
            status = pam_acct_mgmt(handle, 0);
        }
        pam_end(handle, status);
        match status {
            SUCCESS => Outcome::Accepted,
            // PAM_AUTH_ERR, PAM_USER_UNKNOWN, PAM_MAXTRIES, PAM_ACCT_EXPIRED, PAM_PERM_DENIED, PAM_NEW_AUTHTOK_REQD, PAM_CRED_INSUFFICIENT, PAM_AUTHINFO_UNAVAIL
            6..=13 => Outcome::Refused,
            other => Outcome::Broken(format!("pam returned {other}")),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn an_unknown_user_or_a_wrong_password_is_refused_never_accepted() {
        // The service name is one that does not exist, so PAM falls back to "other", which denies everything on a normal system.
        let outcome = authenticate("helix-lock-test-no-such-service", "no-such-user-helix", "x");
        assert!(
            matches!(outcome, Outcome::Refused | Outcome::Broken(_)),
            "{outcome:?}"
        );
        assert_eq!(
            authenticate("helix-lock-test", "a\0b", "x"),
            Outcome::Refused,
            "a NUL anywhere is refused before PAM is asked"
        );
    }

    /// A real PAM stack, written to a temporary file and read from a folder of its own: `pam_exec` hands the typed password to a script, which accepts one value.
    /// Skipped on a machine without `pam_exec`.
    #[test]
    fn the_right_password_is_accepted_and_another_refused_through_a_real_pam_stack() {
        use std::os::unix::fs::PermissionsExt;
        let Some(module) = [
            "/usr/lib64/security/pam_exec.so",
            "/lib64/security/pam_exec.so",
            "/usr/lib/security/pam_exec.so",
            "/usr/lib/x86_64-linux-gnu/security/pam_exec.so",
        ]
        .into_iter()
        .find(|p| std::path::Path::new(p).exists()) else {
            return;
        };
        let dir = std::env::temp_dir().join(format!("helix-lock-pam-{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let script = dir.join("check.sh");
        std::fs::write(
            &script,
            "#!/bin/sh\ntyped=$(tr -d '\\0\\n')\n[ \"$typed\" = \"correct horse\" ]\n",
        )
        .unwrap();
        std::fs::set_permissions(&script, std::fs::Permissions::from_mode(0o755)).unwrap();
        let service = dir.join("helix-lock-test");
        std::fs::write(
            &service,
            format!(
                "auth required {module} expose_authtok quiet {}\naccount required pam_permit.so\n",
                script.display()
            ),
        )
        .unwrap();
        let folder = dir.to_string_lossy().into_owned();
        let user = std::env::var("USER").unwrap_or_else(|_| "root".into());
        assert_eq!(
            authenticate_in(Some(&folder), "helix-lock-test", &user, "correct horse"),
            Outcome::Accepted
        );
        // pam_exec reports a script that said no as a system error, so a stack like this one answers "broken" rather than "refused": either way, not in.
        assert_ne!(
            authenticate_in(Some(&folder), "helix-lock-test", &user, "wrong horse"),
            Outcome::Accepted
        );
        assert_ne!(
            authenticate_in(Some(&folder), "helix-lock-test", &user, ""),
            Outcome::Accepted
        );
        let _ = std::fs::remove_dir_all(&dir);
    }
}
