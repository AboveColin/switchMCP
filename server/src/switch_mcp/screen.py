"""Screenshot post-processing: downscaling and change detection.

A raw console screenshot is a 1280x720 JPEG, roughly 100 KB, and every one of
them lands in the model's context as base64. That is by far the most expensive
thing this server does per call, and most of the cost buys nothing: reading a
menu, confirming a button press or checking whether a transition finished all
work fine at a quarter of the pixels.

Two savings, both applied here rather than on the console — the transfer is
cheap compared to the tokens, and the agent has no image library:

  * downscale/recompress before returning
  * a perceptual hash so an unchanged screen can be reported as "no change"
    instead of resending an identical picture
"""

from __future__ import annotations

import io

from PIL import Image as PILImage

# Native handheld resolution. Tap/swipe coordinates are always in this space,
# so anything scaled must be described in terms of it to stay usable.
NATIVE_W, NATIVE_H = 1280, 720


def downscale(jpeg: bytes, scale: float = 1.0, quality: int = 80) -> bytes:
    """Resize and recompress a JPEG.

    `scale` is a fraction of native resolution (0.5 -> 640x360). Returns the
    input untouched when no work is requested, so the common path costs nothing.
    """
    scale = max(0.1, min(scale, 1.0))
    quality = max(20, min(quality, 95))
    if scale >= 1.0 and quality >= 95:
        return jpeg

    img = PILImage.open(io.BytesIO(jpeg))
    if scale < 1.0:
        img = img.resize(
            (max(1, int(img.width * scale)), max(1, int(img.height * scale))),
            PILImage.LANCZOS,
        )
    out = io.BytesIO()
    img.convert("RGB").save(out, format="JPEG", quality=quality, optimize=True)
    return out.getvalue()


def phash(jpeg: bytes, size: int = 16) -> int:
    """Cheap perceptual hash: mean-threshold over a downsampled greyscale grid.

    Not a real DCT hash — it only has to answer "is this the same screen?", and
    it must not report a change for JPEG noise or a blinking cursor. Returns a
    size*size-bit integer.
    """
    img = PILImage.open(io.BytesIO(jpeg)).convert("L").resize(
        (size, size), PILImage.LANCZOS
    )
    # get_flattened_data() on Pillow >= 11, getdata() before it.
    px = list(img.get_flattened_data() if hasattr(img, "get_flattened_data")
              else img.getdata())
    avg = sum(px) / len(px)
    bits = 0
    for i, p in enumerate(px):
        if p > avg:
            bits |= 1 << i
    return bits


def hamming(a: int, b: int) -> int:
    return bin(a ^ b).count("1")


def looks_same(a: int, b: int, tolerance: int = 8) -> bool:
    """Whether two hashes represent the same screen.

    The default tolerance absorbs compression noise and small animated details
    (a pulsing highlight, a clock ticking) while still catching a menu change.
    """
    return hamming(a, b) <= tolerance
