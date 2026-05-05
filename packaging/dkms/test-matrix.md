# DKMS test matrix

Where the SocksDirect LKM has been built and loaded successfully.

## Tested

| Distro       | Kernel        | Status | Notes                                       |
|--------------|---------------|--------|---------------------------------------------|
| Ubuntu 22.04 | 5.15.0 LTS    | builds | Default LTS target.                         |
| Ubuntu 24.04 | 6.8.0         | builds | The reference target for new development.   |

`builds` here means: `dkms install socksdirect/<ver>` succeeds and
`modprobe socksdirect` returns 0; `/dev/socksdirect` appears with
mode 0660 and group `socksdirect`. The functional ioctl tests run
green against `SD_IOC_GET_VERSION`, `SD_IOC_ECHO`, `SD_IOC_ALLOC_PHYS`,
`SD_IOC_FREE_PHYS`. The page-table-rewrite ioctls
(`SD_IOC_VIRT2PHYS{,_VEC}`, `SD_IOC_MAP_PHYS{,_VEC}`) return
`-ENOSYS`; this is not a regression — see `docs/MISSING_FEATURES.md`.

## Untested but expected to work

Stock kernels 5.10 → 6.10 use the same `cdev` / `ioctl` /
`alloc_pages` / `class_create` / `device_create` interfaces the
module relies on. Reports welcome.

## Known incompatibilities

- Kernels < 5.10: `class_create` signature differs (single argument
  vs. two-argument). The skeleton uses the two-argument form; would
  need a `LINUX_VERSION_CODE` shim.
- Kernels with `CONFIG_STATIC_USERMODEHELPER=y`: untested.

## How to add a row

After running on a new kernel, append a row with the kernel version
string from `uname -r`, the distro, and what level of functionality
you exercised. Include a CI link if the result was produced in CI;
otherwise note "manual" so future readers know who to ask for the
log.
