"""Tests for screenshot downscaling and change detection."""

from __future__ import annotations

import io

from PIL import Image as PILImage

from switch_mcp.screen import downscale, hamming, looks_same, phash


def jpeg(w=1280, h=720, colour=(40, 90, 160), boxes=()):
    img = PILImage.new("RGB", (w, h), colour)
    for (x0, y0, x1, y1, c) in boxes:
        for x in range(x0, x1):
            for y in range(y0, y1):
                img.putpixel((x, y), c)
    out = io.BytesIO()
    img.save(out, format="JPEG", quality=90)
    return out.getvalue()


def test_downscale_shrinks_and_stays_decodable():
    src = jpeg()
    small = downscale(src, 0.5)
    img = PILImage.open(io.BytesIO(small))
    assert (img.width, img.height) == (640, 360)
    assert len(small) < len(src)


def test_downscale_is_a_noop_at_full_quality():
    src = jpeg()
    assert downscale(src, 1.0, 95) is src


def test_scale_is_clamped_to_sane_bounds():
    src = jpeg()
    tiny = PILImage.open(io.BytesIO(downscale(src, 0.001)))
    assert tiny.width >= 1 and tiny.height >= 1
    big = PILImage.open(io.BytesIO(downscale(src, 99.0, 80)))
    assert big.width == 1280  # never upscales past native


def test_identical_screens_hash_the_same():
    src = jpeg()
    assert phash(src) == phash(src)
    assert looks_same(phash(src), phash(src))


def test_recompression_noise_does_not_register_as_a_change():
    """A re-encoded frame of the same screen must not look 'changed', or every
    poll would report a false positive."""
    src = jpeg()
    noisy = downscale(src, 1.0, 60)  # same picture, different compression
    assert looks_same(phash(src), phash(noisy))


def test_a_real_ui_change_is_detected():
    before = jpeg()
    after = jpeg(boxes=[(100, 100, 700, 500, (250, 250, 250))])  # a dialog appears
    assert not looks_same(phash(before), phash(after))


def test_small_detail_change_stays_under_tolerance():
    """A tiny changing element (a clock digit) should not trip change detection."""
    before = jpeg()
    after = jpeg(boxes=[(1240, 10, 1260, 30, (255, 255, 255))])
    assert looks_same(phash(before), phash(after))


def test_hamming_is_symmetric_and_zero_for_equal():
    a, b = phash(jpeg()), phash(jpeg(colour=(200, 30, 30)))
    assert hamming(a, a) == 0
    assert hamming(a, b) == hamming(b, a)
