{
  pkgs,
  sources,
  dependencies,
  target ? "host",
  configuration ? "release",
  toolchain ? { },
  schemaVersion ? 1,
}:
assert toolchain == { };
assert schemaVersion == 1 && target == "host";
let
  base = pkgs.qemu.override {
    hostCpuTargets = [ "x86_64-softmmu" ];
    virglrenderer = dependencies.renderer;
    virglSupport = true;
    openGLSupport = true;
    sdlSupport = true;
    vncSupport = true;
    gtkSupport = false;
    rutabagaSupport = false;
    enableDocs = false;
    guestAgentSupport = false;
    spiceSupport = false;
    usbredirSupport = false;
    smartcardSupport = false;
    brlttySupport = false;
    libiscsiSupport = false;
    pulseSupport = false;
    pipewireSupport = false;
    jackSupport = false;
  };
in
base.overrideAttrs (old: {
  pname = "qemu-helios";
  version = "11.1.1";
  src = sources.qemu-helios;
  buildInputs = old.buildInputs ++ [
    pkgs.vulkan-loader
    pkgs.vulkan-headers
  ];
  patches = [ ];
  # The fixed upstream tarball supplies keycodemapdb, not the QEMU executable.
  # Refuse a changed wrap identity rather than silently using another revision.
  postPatch = ''
    tar -xf ${pkgs.qemu.src} --strip-components=1 --wildcards '*/subprojects/keycodemapdb*'
    cmp subprojects/keycodemapdb.wrap ${sources.qemu-helios}/subprojects/keycodemapdb.wrap
    tar -xf ${pkgs.qemu.src} --strip-components=1 --wildcards '*/subprojects/berkeley-*'
    for wrap in subprojects/berkeley-*.wrap; do
      cmp "$wrap" "${sources.qemu-helios}/$wrap"
    done
    mkdir -p third_party/Vulkan-Headers
    cp -r ${pkgs.vulkan-headers}/include third_party/Vulkan-Headers/
  '';
  configureFlags =
    # An empty cross prefix still makes QEMU configure declare a cross build,
    # which causes Meson to skip every executable unit test on a native host.
    (pkgs.lib.filter (flag: flag != "--cross-prefix=") old.configureFlags)
    ++ [
      "--disable-download"
      "--enable-kvm"
      "--enable-modules"
      "--enable-sdl"
      "--enable-vnc"
      "--disable-werror"
      "--disable-rust"
    ]
    ++ pkgs.lib.optional (configuration == "debug") "--enable-debug";
  doCheck = true;
  preCheck = "";
  checkPhase = ''
    runHook preCheck
    meson test -C build --suite unit --print-errorlogs
    ${pkgs.python3}/bin/python - <<'PY'
    import json
    from pathlib import Path
    tests = [json.loads(line) for line in Path("build/meson-logs/testlog.json").read_text().splitlines()]
    assert len(tests) > 100, "QEMU unit suite was not exercised"
    allowed_skips = {"test-seccomp", "test-yank", "test-nested-aio-poll"}
    assert sum(test["result"] == "OK" for test in tests) >= 105, "QEMU unit executables were skipped"
    assert all(test["result"] == "OK" or (test["result"] == "SKIP" and test["name"].split(":")[-1] in allowed_skips) for test in tests), "unexpected QEMU unit skip/failure"
    PY
    runHook postCheck
  '';
  postInstall = ''
    mkdir -p $out/share/licenses/qemu-helios
    cp COPYING COPYING.LIB $out/share/licenses/qemu-helios/
    cp pc-bios/README $out/share/licenses/qemu-helios/firmware-README
    find pc-bios -maxdepth 1 -iname '*license*' -exec cp '{}' $out/share/licenses/qemu-helios/ ';'
    cp -r pc-bios/descriptors $out/share/qemu/firmware-descriptors
    mkdir -p $out/share/build-validation/qemu-helios
    cp build/meson-logs/testlog.json build/meson-logs/testlog.txt $out/share/build-validation/qemu-helios/
    # Test commands include the debug-prefix map. Keep the reports without an
    # out -> debug reference cycle; the artifact manifest identifies debug.
    substituteInPlace $out/share/build-validation/qemu-helios/testlog.{json,txt} \
      --replace-warn "$debug" '@debug-output@'
  '';
  requiredSystemFeatures = [ ];
})
