# Rendering and images

## Shared and application-owned code

`lib/` is a normal local copy of Noctalia's reusable library. Do not patch it in Luciole. Port generic fixes upstream before copying reviewed updates back; do not introduce a sibling include path, shell service or second renderer.

[library_setup.cpp](../../src/app/library_setup.cpp) supplies application identity, explicit asset roots, palette, UI strings and the Luciole image provider. Luciole selects its canonical environment/install asset root before library initialization. An invalid explicit root fails rather than silently searching elsewhere.

Luciole owns decoration negotiation, clipboard/drop transport, cursor fallback, browser composition and selection contours. Common widgets use the actual shared Button, Stepper, Select, Toggle, ScrollView and virtual-grid controls. A file-manager preference binding belongs here, while a generic widget correction belongs upstream.

## UI phases

Preserve PrepareFrame → Update → Layout → Render.

- Update applies prepared state and publishes model/UI changes.
- Layout measures, mutates scene structure, binds visible cells and chooses physical image sizes.
- Render only paints prepared state. It performs no layout, traversal, filesystem I/O or decoding.

Callbacks schedule the required phase. They do not synchronously rebuild a scene from an arbitrary worker or input callback. Virtual cells release work when hidden, retired or rebound; bounded pooling must not retain every file in a large directory.

## Icon theme updates

Luciole owns icon selection and monitoring in `src/system/icon_resolver.*` and `icon_theme_poll_source.*`. Desktop GSettings selects the theme when its schema is available; GTK settings.ini provides a fallback. The main-loop watcher observes `changed::icon-theme`, reads the key after connecting, and debounces selection changes alongside existing artwork file watches. Accepted changes invalidate the application icon cache and rebind browser artwork. GSettings objects are optional when the schema is absent; observer teardown disconnects callbacks. The `icon-theme` suite uses private schemas/settings and artwork to exercise selection, fallback, live changes and file replacement.

## Thumbnail and preview pipeline

[ThumbnailService](../../src/thumbnails/thumbnail_service.h) owns bounded generation requests, revisions, visibility priority and cancellation. [PreviewService](../../src/thumbnails/preview_service.h) requests appropriate preview content. Their generation policy, freedesktop disk store and FFmpeg/Poppler providers remain Luciole-owned.

Raster images use the [GdkPixbuf/Glycin adapter](../../src/imaging/pixbuf_image_decoder.cpp). Decoding/provider processes run outside Render with bounded input, pixels, memory and time. [thumbnail_worker.cpp](../../src/thumbnails/thumbnail_worker.cpp) and [provider_process.cpp](../../src/thumbnails/provider_process.cpp) enforce the helper protocol; [thumbnail_store.cpp](../../src/thumbnails/thumbnail_store.cpp) validates shared PNG cache entries. Dependencies and installed loader coverage belong in [PACKAGING.md](../../PACKAGING.md).

[FileIcon](../../src/ui/browser/file_icon.h) holds shared-library prepared-image leases. Immutable RGBA and source/revision/size identities enter [SharedTextureCache](../../lib/render/core/shared_texture_cache.h). Dispatch owns GPU upload and switching; identical prepared requests share residency within an EGL group. Do not reinstate per-cell texture uploads or a second Luciole GPU cache.

Layout requests physical pixels, including display scale. Grid thumbnails cannot replace the larger preview request. Keep an already ready image visible until a higher-resolution replacement uploads, preserve aspect ratio, and retain source dimensions separately from decoded/output dimensions. Source revisions must invalidate same-size edits without flushing unrelated images.

Preview and Properties share [asynchronous metadata](../../src/thumbnails/preview_metadata.h). The preview presents essential content facts; Properties can expose technical and permission information. Recursive folder totals are a separate explicit Properties service, never a side effect of selecting an entry.

## Verification

Use [thumbnail](../../tests/thumbnail_test.cpp), [preview](../../tests/preview_service_test.cpp), [metadata](../../tests/preview_metadata_test.cpp) and [icon](../../tests/file_icon_test.cpp) suites for deterministic contracts. Verify image sizing, placeholder transitions, scrolling, fractional scale and pane resizing in an isolated actual window. Loader failures, graphics-reset injection and hardware/monitor coverage must be reported at the level actually tested; old successful CPU fixtures are not proof of GPU recovery or universal format coverage.
