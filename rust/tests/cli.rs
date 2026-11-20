use std::fs;
use std::path::{Path, PathBuf};
use std::process::{Command, Output};
use std::sync::atomic::{AtomicU32, Ordering};

struct Fixture(PathBuf);

impl Fixture {
    fn new() -> Self {
        static ID: AtomicU32 = AtomicU32::new(0);
        let dir = std::env::temp_dir().join(format!(
            "disk-diver-rust-{}-{}",
            std::process::id(),
            ID.fetch_add(1, Ordering::Relaxed)
        ));
        fs::create_dir(&dir).unwrap();
        Self(dir)
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        let _ = fs::remove_dir_all(&self.0);
    }
}

fn run(command: &str, path: &Path) -> Output {
    Command::new(env!("CARGO_BIN_EXE_disk-diver-rust"))
        .arg(command)
        .arg(path)
        .output()
        .unwrap()
}

fn success(output: Output) -> Vec<u8> {
    assert!(
        output.status.success(),
        "{}",
        String::from_utf8_lossy(&output.stderr)
    );
    output.stdout
}

#[test]
fn scan_summary_and_replay_agree_for_hardlinks() {
    let workspace = Fixture::new();
    let tree = workspace.0.join("tree");
    fs::create_dir_all(tree.join("sub")).unwrap();
    fs::write(tree.join("alpha"), b"1234567").unwrap();
    fs::hard_link(tree.join("alpha"), tree.join("sub/beta")).unwrap();
    fs::write(tree.join("sub/independent"), b"123").unwrap();
    let summary = String::from_utf8(success(run("summary", &tree))).unwrap();
    for field in [
        "\"directories\":2",
        "\"files\":3",
        "\"logical\":10",
        "\"complete\":true",
    ] {
        assert!(summary.contains(field), "{summary}");
    }
    let capture = workspace.0.join("capture");
    fs::write(&capture, success(run("scan", &tree))).unwrap();
    assert_eq!(success(run("replay", &capture)), summary.as_bytes());
    fs::write(&capture, success(run("list", &tree))).unwrap();
    let list = String::from_utf8(success(run("replay", &capture))).unwrap();
    for field in ["\"directories\":2", "\"files\":1", "\"logical\":7"] {
        assert!(list.contains(field), "{list}");
    }
}

#[test]
fn invalid_roots_or_truncated_captures_fail() {
    let dir = Fixture::new();
    let missing = run("scan", &dir.0.join("missing"));
    assert!(!missing.status.success());
    assert!(!missing.stdout.ends_with(&[5]));
    let capture = dir.0.join("capture");
    for malformed in [
        b"DDV1".as_slice(),
        b"DDV1\x05",
        b"DDV1\x01\xff",
        b"DDV1\x05unexpected",
    ] {
        fs::write(&capture, malformed).unwrap();
        assert!(!run("replay", &capture).status.success());
    }
}

#[cfg(target_os = "linux")]
#[test]
fn raw_names_survive_and_directory_symlinks_are_leaves() {
    use std::ffi::OsStr;
    use std::os::unix::ffi::OsStrExt;
    let dir = Fixture::new();
    fs::write(dir.0.join(OsStr::from_bytes(b"bad-\xff")), b"123").unwrap();
    std::os::unix::fs::symlink(&dir.0, dir.0.join("loop")).unwrap();
    let scan = success(run("scan", &dir.0));
    assert!(scan.windows(5).any(|s| s == b"bad-\xff"));
    let summary = String::from_utf8(success(run("summary", &dir.0))).unwrap();
    assert!(summary.contains("\"other\":1"));
    assert!(summary.contains("\"logical\":3"));
}
