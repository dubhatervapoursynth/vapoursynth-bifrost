Description
===========

Bifrost is a temporal derainbowing filter created by Fredrik Mellbin.

The original Avisynth plugin (version 1.1) worked on the whole frame or not at all. This version works on blocks, meaning that static parts of the image can be processed even if something moves on screen.

The VapourSynth plugin will accept any 8 bit YUV formats. The Avisynth plugin is still limited to YV12.


Usage
=====

::

   bifrost.Bifrost(clip clip[, clip altclip=clip, float luma_thresh=10.0, int variation=5, bint conservative_mask=False, bint interlaced=True, int blockx=4, int blocky=4])

- altclip

  Bifrost will copy from this clip the chroma of the blocks it can't process. This allows moving blocks to be processed with some other filter. *altclip* must have the same format, dimensions and length as *clip*.

- luma_thresh

  Blocks where the luma changes less than this are considered static and may be processed.

- variation

  Controls how big a chroma change must be in order to be considered a rainbow. Increasing this can reduce false positives.

- conservative_mask

  If true, only pixels detected as rainbows will be processed. Otherwise, pixels that have rainbows above and below them will also be processed.

- interlaced

  If true, SeparateFields, DoubleWeave and SelectEvery will be invoked in order to process the top and bottom fields separatedly.

- blockx

- blocky

  The dimensions of the blocks. Smaller is probably better. The default 4×4 should be good enough. They must be a multiple of the clip's subsampling, at least two chroma samples wide and tall, and no larger than the frame.

Note: If the dimensions of the image aren't divisible by *blockx* and *blocky*, the right and/or bottom edges won't be processed. The chroma of those edges is copied from *altclip*, same as for any other block Bifrost can't process.


Types of rainbows
=================

When dealing with telecined material you must determine if the rainbows were added before or after the telecine process. Find a scene with clearly visible rainbows and use the following::

   src = your.Source()
   separated = std.SeparateFields(src)
   even = std.SelectEvery(separated, 2, 0)

If the rainbows change every frame, they were added after telecine. If there is a duplicate every five frames, they were added before.

Rainbows added before telecine
------------------------------

First you must fix the telecine, then use Bifrost in progressive mode::

   ret = d2v.Source("rainbows1.d2v")
   ret = vivtc.VFM(ret, order=1, chroma=False, mchroma=False)
   ret = vivtc.VDecimate(ret, chroma=False)
   ret = bifrost.Bifrost(ret, interlaced=False)


Rainbows added after telecine
-----------------------------

In this case, Bifrost should be used in interlaced mode, right after the source filter::

   ret = d2v.Source("rainbows2.d2v")
   ret = bifrost.Bifrost(ret, interlaced=True)
   # ivtc here


Installation
============

::

    pip install vapoursynth-bifrost


Compilation
===========

Requires Meson >= 1.2.3 and a C++17 compiler. VapourSynth must be installed and
importable by the Python that Meson finds, because the include path is queried with
``vapoursynth.get_include()``. That same interpreter also decides where the plugin
installs (``<site-packages>/vapoursynth/plugins``).

::

    meson setup build
    ninja -C build

If you have several Python installations, point Meson at the right one with a native
file containing::

    [binaries]
    python = '/path/to/python'

and passing ``--native-file`` to ``meson setup``.

To build a wheel instead::

    python -m build

DLLs for Avisynth 2.5.8+ can be found in the "releases" section.


License
=======

The license is WTFPL; see the LICENSE file.
