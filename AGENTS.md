# Project workflow memory

- For release-quality UEFI builds, prefer the `Build CI` GitHub Actions workflow
  in `feice11/gbl_root_canoe` over attempting to reproduce the Linux toolchain
  on the Windows host.
- After a successful Actions run, download the build artifacts back into
  `artifacts/github-actions/<run-id>/` in this workspace and report the exact
  paths to the user.
- Keep the LinuxLoader UI layer separate from boot discovery and launch logic.
  New screens should reuse `SfbBeginScreen`, `SfbDrawRow`, `SfbEndScreen`, and
  `SfbReportStatus` so navigation and visual behavior remain consistent.
- The handset input model is Volume Up/Down for navigation and Power for
  selection. Preserve a text-console fallback for Qualcomm firmware
  compatibility if a graphical UI is added later.
