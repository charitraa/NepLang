# Packaging NepLang

Every pushed `v*` tag runs `.github/workflows/release.yml`, which builds the binaries and
`.deb`/`.rpm` packages, publishes the GitHub Release, and then (when the secrets below are
set) updates Homebrew, Scoop and winget. `render.sh VERSION SHA256SUMS OUT_DIR` fills in the
`*.in` templates from a release's checksum file; the workflow runs it, and you can run it by
hand with the `SHA256SUMS` attached to any release.

| Package manager | Users run | One-time setup | Then |
|---|---|---|---|
| Homebrew (macOS, Linux) | `brew install charitraa/neplang/neplang` | repo `charitraa/homebrew-neplang` + secret `PACKAGING_TOKEN` | automatic |
| Scoop (Windows) | `scoop bucket add neplang https://github.com/charitraa/scoop-neplang` then `scoop install neplang` | repo `charitraa/scoop-neplang` + the same secret | automatic |
| winget (Windows) | `winget install charitraa.NepLang` | first version submitted by hand to `microsoft/winget-pkgs` + secret `WINGET_TOKEN` | automatic PR, reviewed by Microsoft |
| apt via PPA (Ubuntu) | `sudo add-apt-repository ppa:charitraa/neplang` then `sudo apt install neplang` | Launchpad account, PPA, GPG key | run `ppa/upload.sh VERSION` per release |

## Homebrew and Scoop

1. Create the two public repos, each with the current release's file
   (`homebrew-neplang/Formula/neplang.rb`, `scoop-neplang/bucket/neplang.json`, rendered by
   `render.sh`), on branch `main`.
2. Create a fine-grained token at <https://github.com/settings/personal-access-tokens/new>:
   repository access = only `homebrew-neplang` and `scoop-neplang`, permission
   **Contents: Read and write**.
3. Save it in the NepLang repo: `gh secret set PACKAGING_TOKEN -R charitraa/NepLang`
   (paste the token when asked).

Without the secret, the `homebrew-scoop` job still renders the files but skips the push.

## winget

1. Submit the first version by hand: fork `microsoft/winget-pkgs`, add the three rendered
   manifests under `manifests/c/charitraa/NepLang/VERSION/`, and open a pull request.
   (Or use `wingetcreate new` on Windows.) Microsoft's bots validate it; a human approves
   it, usually within a few days.
2. Once it is merged, create a classic token with the `public_repo` scope and save it:
   `gh secret set WINGET_TOKEN -R charitraa/NepLang`. Later releases then open the update
   PR automatically (job `winget`, using `vedantmgoyal9/winget-releaser`). The token's
   account needs its own fork of `microsoft/winget-pkgs`.

## Ubuntu PPA

1. Create an account at <https://launchpad.net>, sign the Ubuntu Code of Conduct, and
   create a PPA named `neplang`.
2. Make a GPG key (`gpg --full-generate-key`), upload it
   (`gpg --keyserver keyserver.ubuntu.com --send-keys KEYID`) and add its fingerprint to
   your Launchpad profile.
3. Install the tools: `sudo apt install devscripts debhelper dput`.
4. For each release: add the version to `debian/changelog`, then run
   `packaging/ppa/upload.sh VERSION`. Launchpad builds it for each Ubuntu release.

## Arch

`arch/PKGBUILD` builds from the git checkout: `cd packaging/arch && makepkg -si`.
Publishing it to the AUR is a separate, optional step.
