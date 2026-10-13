# qemu-helios build interface

`default.nix` accepts schemaVersion 1, `pkgs`, explicit `sources`,
`dependencies`, `target`, `configuration` (release/debug) and `toolchain`.
It returns a derivation for native/cross outputs or a devbox dispatch record
for ABI constrained Windows targets. Dependencies are immutable Nix output
paths. Sources are exported snapshots, including selected gitlink contents.
Only locked toolchains are accepted (`toolchain = {}`); nonempty overrides
are refused. No recipe downloads dependencies during compilation.

The committed devenv inputs/lock match the environment workspace. From this
checkout run `devenv shell -- wb-component-build /path/to/spec.json`. The JSON
spec supplies system, schemaVersion, sources (`id: {path: ..., narHash: "sha256-..."}`), dependencies
(`id: /nix/store/...`), target and configuration explicitly. Nix therefore never
assumes the location of another checkout. The environment's `wb build` prepares
these snapshots and records full source/toolchain/artifact provenance.

Guest dispatch records require Stage 4's local disk mirror and durable elevated
backend. They are plans, with runtime verification pending; MinGW outputs cannot
substitute for an MSVC static engine. Licenses and debug symbols must accompany
exported artifacts.

The runtime output retains the complete Meson unit reports as
`share/build-validation/qemu-helios/testlog.json.gz` and `testlog.txt.gz`.
They are losslessly compressed with timestamp-free gzip headers so their
recorded build environment does not make the compiler, build tools or source
archives runtime dependencies. Nix rejects those dependencies in the runtime
output. Debug symbols and their source references remain in the separate debug
output and accompany exported artifacts.

Only x86 EDK2 images accompany the x86_64-softmmu build; images for ARM, RISC-V
and LoongArch are excluded. Component CI publishes QEMU's own executable,
modules, data, symbols and validation reports. Runtime dependency paths and
hashes are recorded in build provenance; this component artifact is not a
self-contained host environment.
