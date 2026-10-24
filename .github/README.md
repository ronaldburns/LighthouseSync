# LighthouseSync

Lighthouse trackers, controllers and base stations lined up with a rooted
Quest's or a Steam Frame's own tracking, with nothing mounted on the headset.
The headset's tracking cameras see your SteamVR base stations, and that keeps
the lighthouse space aligned with the headset's while you play.

LighthouseSync is a fork of [QuestLHSync](https://github.com/CreoleVR/QuestLHSync)
by CreoleVR. The tracking is QuestLHSync's, and the fork follows its updates.
What the fork adds:

- **A dashboard** that opens as a window on the desktop and as a page in the
  SteamVR dashboard: the headset connection, every base station (signal, power,
  which cameras see it), the alignment, settings, and developer pages for the
  cameras and the driver's log.
- **Base station power:** wake 2.0 base stations up and put them to sleep over
  Bluetooth.
- **One-line installs** for the PC and the Steam Frame, checked against each
  release's SHA-256 checksums.
- **Updates everywhere:** the dashboard installs new versions on the PC, the
  Steam Frame updates itself in the background, and Magisk updates the Quest
  module.
- **A release for every change** to `master`, with build provenance
  attestations.

LighthouseSync installs where QuestLHSync does and keeps its file names, so it
replaces an existing QuestLHSync in place and keeps your saved base stations and
settings.

## Install

You need what QuestLHSync needs (see its
[requirements](../README.md#requirements)): a Quest Pro, Quest 3 or Quest 3S
rooted with Magisk, or a Steam Frame; SteamVR base stations; Windows with
SteamVR; the PC and the headset on the same network; and at least one lighthouse
device switched on.

**PC:** run this in PowerShell.

```powershell
irm https://github.com/ronaldburns/LighthouseSync/releases/latest/download/install.ps1 | iex
```

It installs the driver into `%LOCALAPPDATA%\QuestLHSync\questlhsync`, registers
it with SteamVR and turns on SteamVR's `activateMultipleDrivers` setting if it
isn't on yet (it asks to close SteamVR for that). Otherwise SteamVR can stay
open, and the new version loads the next time it starts.

**Steam Frame:** run this in a terminal in desktop mode, or over ssh.

```sh
curl -fsSL https://github.com/ronaldburns/LighthouseSync/releases/latest/download/install-frame.sh | bash
```

SteamVR on the headset restarts once to load its driver. To install without
automatic updates, end the command with `bash -s -- --no-auto-update` instead.

**Quest:** download `LighthouseSync-quest-module-<version>.zip` from the
[latest release](https://github.com/ronaldburns/LighthouseSync/releases/latest),
install it in the Magisk app (**Modules > Install from storage**) and reboot.

Then turn off SpaceCalibrator, OpenVR-SpaceSync or anything else that moves
lighthouse devices. Two tools correcting the same devices fight each other.

[Use](../README.md#use), [settings](../README.md#settings) and
[troubleshooting](../README.md#troubleshooting) are as in QuestLHSync.

## Updates

- **PC:** the dashboard looks for a new release when SteamVR starts and every
  6 hours, and the Updates page shows what's new and installs it. Nothing
  installs until you choose to. The Updates page's **Update source** is the
  repository releases come from, so a fork of LighthouseSync can follow its own
  releases. Releases built by a fork's own workflow point at that fork already.
- **Steam Frame:** a systemd user timer looks for a new release every 6 hours,
  downloads it and checks it, and the new version installs the next time the
  headset starts, before its SteamVR does. Run the install command again to
  update right away. `systemctl --user list-timers lighthousesync-update.timer`
  shows the next check.
- **Quest:** open Magisk, go to **Modules** and tap **Update** on LighthouseSync.

## Uninstall

- **PC** (keeps your saved base stations and settings in
  `%LOCALAPPDATA%\QuestLHSync`):

  ```powershell
  & ([scriptblock]::Create((irm https://github.com/ronaldburns/LighthouseSync/releases/latest/download/install.ps1))) -Uninstall
  ```

- **Steam Frame:**

  ```sh
  curl -fsSL https://github.com/ronaldburns/LighthouseSync/releases/latest/download/install-frame.sh | bash -s -- --uninstall
  ```

- **Quest:** remove the module in the Magisk app and reboot.

## Checking the downloads

Every release has a `SHA256SUMS` file, and the installers and the dashboard
check what they download against the SHA-256 GitHub publishes for each file, or
against `SHA256SUMS`. Every file also has a build provenance attestation, which
proves this repository's Release workflow built it from the commit it names:

```sh
gh attestation verify LighthouseSync-steamvr-1.26.1.zip --repo ronaldburns/LighthouseSync
```

## How the fork works

- **Releases:** every push to `master` that changes more than documentation runs
  the Release workflow, which builds everything on GitHub's runners and
  publishes `v<QuestLHSync version>.<n>`: v1.26.1, v1.26.2 and so on, then
  v1.27.1 after QuestLHSync 1.27 is merged.
- **Pull requests** run the Build workflow. Its artifacts have a development
  build (which doesn't update itself) and PNGs of every dashboard page.
- **Upstream Sync** checks QuestLHSync every day and opens a pull request with
  its new commits, built. Merge it with **Create a merge commit**, so the next
  sync doesn't conflict with rewritten commits.
- QuestLHSync's internal names stay: the `questlhsync` folders, the DLL and exe
  names, the data folder, the Frame's `questlhsync.service` and the Magisk
  module's id. That keeps merges from QuestLHSync clean and lets installs
  upgrade in place.

### Code signing

The Build workflow signs the driver and the dashboard with
[Azure Artifact Signing](https://learn.microsoft.com/azure/artifact-signing/)
when the repository has these secrets:

| Secret | What it is |
| --- | --- |
| `AZURE_TENANT_ID`, `AZURE_CLIENT_ID`, `AZURE_CLIENT_SECRET` | An app registration with the Certificate Profile Signer role on the signing account |
| `AZURE_SIGNING_ENDPOINT` | The signing account's region endpoint, like `https://eus.codesigning.azure.net/` |
| `AZURE_SIGNING_ACCOUNT` | The signing account's name |
| `AZURE_CERTIFICATE_PROFILE` | The certificate profile's name |

Without them, releases aren't signed and work the same.

## Building

As in QuestLHSync (see [Building](../README.md#building)), plus:

```
python lhs\stamp.py v1.26.3 [owner/name]    stamp a version into the builds; --clear for a local build again
QuestLHSync.exe --preview <dir> [locked|acquiring|searching|stale]   PNGs of the dashboard pages, sample data
QuestLHSync.exe --selftest <file>           checks the dashboard without showing a window or changing SteamVR
python lhs\package.py v1.26.3 owner/name    the release files, into out\lhs-release\
python lhs\make_icon.py                     lhs\icon.ico from the dashboard's own drawing
```

`lhs/package.py` needs the headset builds made with the same version and
repository, from the `LHS_VERSION` and `LHS_REPO` environment variables, as the
Build workflow does. A build that isn't stamped calls itself a development
build and doesn't update itself.

The dashboard uses [Dear ImGui](https://github.com/ocornut/imgui) and the
[IBM Plex](https://github.com/IBM/plex) fonts, in `third_party/`. Their notices
are in [lhs/THIRD_PARTY_NOTICES.md](../lhs/THIRD_PARTY_NOTICES.md).

## License

MIT, like QuestLHSync: see [LICENSE](../LICENSE).

LighthouseSync is not affiliated with CreoleVR, Meta or Valve. Meta Quest is a
trademark of Meta Platforms, Inc.; Steam Frame and SteamVR are trademarks of
Valve Corporation. Rooting a headset and running code inside its system
services is at your own risk.
