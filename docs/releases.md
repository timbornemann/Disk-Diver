# Publishing Disk-Diver on GitHub

The release workflow builds the Windows x64, Debian/Ubuntu amd64, and Fedora x86_64 packages from the same Git commit. A tag such as `v0.2.0` sets the version for all three packages. The workflow also produces `disk-diver-update.json` and its detached Ed25519 signature, `disk-diver-update.json.sig`. If a required build or test fails, automatic publication stops. [Release workflow](../.github/workflows/release.yml)

## Set up the update channel

1. In the public GitHub repository, open **Settings → Environments** and create an environment named `release`. Restrict which tags can deploy to it and who can approve releases. Protect release tags with a tag ruleset.

2. Generate an Ed25519 key pair on your computer. This PowerShell example stores the private key in your user profile and the public key in `$publicKey`.

   ```powershell
   python -m pip install cryptography
   $keyFile = Join-Path $env:USERPROFILE 'disk-diver-release-private.pem'
   $publicKey = python packaging/create_update_key.py $keyFile
   ```

3. Set the **repository variable** `DISK_DIVER_UPDATE_PUBLIC_KEY` to the public key. Set the **environment secret** `DISK_DIVER_UPDATE_PRIVATE_KEY` in the `release` environment to the full private PEM key. From a checkout whose `origin` points to the repository, run these commands with an authenticated GitHub CLI.

   ```powershell
   gh variable set DISK_DIVER_UPDATE_PUBLIC_KEY --body $publicKey
   Get-Content -Raw -LiteralPath $keyFile | gh secret set DISK_DIVER_UPDATE_PRIVATE_KEY --env release
   ```

4. Keep a protected backup of the private key and use the same key pair for subsequent releases. An installed app trusts the public key it shipped with. With this workflow, changing the signing key requires a manual installation from an independently verified source.

## Publish a version

Tag the commit you want to release and push the tag. For example, to publish `v0.2.0` from the current commit, run the following commands.

```sh
git tag -a v0.2.0 -m "Disk-Diver 0.2.0"
git push origin v0.2.0
```

Follow the build and package tests under **Actions → Release installers**. The workflow creates a draft release for the tag or uses an existing draft. It uploads the installers, manifest, and signature before publishing. You can prepare release notes in the draft. This sequence supports immutable GitHub releases. [GitHub's guide to immutable releases](https://docs.github.com/en/code-security/concepts/supply-chain-security/immutable-releases)

If your repository allows changes to published releases, you can also publish a stable release through **Releases → Draft a new release → Publish release**. The installers appear when the workflow finishes. With immutable releases enabled, use the tag workflow so all five assets are attached before publication.

To rerun a release for an existing tag, go to **Actions → Release installers → Run workflow**. If your environment restricts deployments to release tags, select the release tag under **Use workflow from** as well. If an upload is interrupted, rerun the failed job with its original build artifacts. Publishing different content under an existing asset name requires a new version.

## How the installed app checks for updates

Disk-Diver checks the latest stable release of the GitHub repository recorded at build time. It checks shortly after startup and every six hours while running. Select **Updates → Check for Updates…** to check manually. The updater only offers stable versions newer than the installed version.

The signed manifest identifies the repository and release version and lists the platform, filename, size, and SHA-256 digest for each installer. The app verifies the signature using its embedded public key, checks the downloaded bytes, and saves the complete package in the user's private cache. It verifies the package again when you choose to install it.

On Windows, Disk-Diver launches the Inno Setup installer. On Linux, the installation helper requests authorization through polkit, verifies the incoming data again in a private directory, and passes that copy to APT or DNF. A failed check stops installation.

Local builds without a configured repository and public key leave network update checks disabled. Release signatures protect updates starting from the first trusted installation. Displaying a verified publisher in the Windows User Account Control prompt also requires an Authenticode certificate for the installer.
