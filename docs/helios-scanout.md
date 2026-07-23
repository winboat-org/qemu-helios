# Helios native Venus scanout

This fork carries the host-display side of the Helios Windows virtio-gpu
scanout path. It does not add or change a public virtio-gpu command.

## Metadata

Virglrenderer is queried with `virgl_renderer_resource_get_info_ext()` when the
installed API supports it. QEMU carries the returned DRM modifier in
`QemuDmaBuf`. It also carries the allocation size already supplied by the guest
in `VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB`; this is internal metadata, not a new
wire field.

Generic LINEAR scanout validation remains strict: offset plus stride times
height must fit the resource. A native `HOST3D` image may have a larger opaque
Vulkan allocation than its visible row span, so it is validated against the
original blob allocation size instead of being rejected as malformed LINEAR
storage.

## Display backends

An explicit DRM modifier or a proven LINEAR export can use QEMU's normal EGL
DMA_BUF import. A plain Vulkan `VK_IMAGE_TILING_OPTIMAL` export can arrive with
`DRM_FORMAT_MOD_INVALID`; EGL then has no layout description and cannot import
it correctly.

For that case, the OpenGL display modules share an exact Vulkan fallback:

1. Recreate the producer image with the known Helios primary shape.
2. Require its Vulkan memory requirement to equal the original blob allocation
   size exactly. A mismatch rejects the fallback rather than aliasing the DMA_BUF
   with the wrong image layout.
3. Import the DMA_BUF as dedicated external image memory.
4. Transfer ownership from the external producer, copy the damaged rectangle to
   host-visible staging memory, and release ownership back.
5. Update QEMU's CPU `DisplaySurface` for VNC or upload it to the normal GTK/SDL
   surface texture.

The fallback is implemented for egl-headless, GTK EGL, GTK GLArea, and SDL
OpenGL. Readback objects are cached by DMA_BUF identity so DWM's primary-buffer
rotation does not recreate Vulkan devices every frame.

This path avoids a guest-side copy and preserves the protocol ABI, but it is not
end-to-end zero-copy: the host copies the optimal image to staging and then to a
QEMU display surface. True zero-copy requires an importable layout contract,
such as a correct explicit DRM modifier, without changing ordinary guest image
imports.

Interactive frontends need two different GPU selections on a hybrid Wayland
host. SDL/GTK must create their EGL context through the compositor's normal EGL
vendor (Mesa/Intel on the Helios development host), while the Vulkan fallback
and Venus renderer remain pinned to NVIDIA with `VK_ICD_FILENAMES` and
`__VK_LAYER_NV_optimus`. Globally forcing NVIDIA EGL prevents a Wayland context
from being created. The Helios launcher applies this split automatically.

## Verification

Build the affected modules together and then ask the QEMU binary to load all
display backends:

```sh
ninja -C build-helios qemu-system-x86_64 ui-opengl.so ui-egl-headless.so \
  ui-gtk.so ui-sdl.so hw-display-virtio-gpu.so \
  hw-display-virtio-gpu-gl.so
QEMU_MODULE_DIR="$PWD/build-helios" \
  ./build-helios/qemu-system-x86_64 -display help
```

egl-headless + VNC and SDL OpenGL on native Wayland have visible-output
verification on NVIDIA. SDL now fails loudly if it cannot create or make
current an EGL-backed GL context. GTK/Wayland builds and initializes, but the
full Windows run currently fails later with repeated GDK `eglMakeCurrent`
errors; it is not a verified frontend yet.
