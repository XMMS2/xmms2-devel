XMMS2 plugin priority system
============================

This document attemps to explain how the XMMS2 plugin priority system works.

As an XMMS2 user, this allows you to control which plugins handle which file
formats.  For example you can control whether libmad or mpg123 are used to
decode MP3 files during playback.

Generally speaking, you don't need any of this information to use XMMS2.  But
if you are playing a less common file format and the wrong plugin is handling
it, this may explain how to get things working the way you want.

Chains, xforms and goals
------------------------

An xform is a plugin that transforms data from one format to another.  The
`file` xform takes a `file://` URL and transforms it into binary data.  The
`mad` xform takes binary data in MP3 format and transforms it into decompressed
PCM audio samples.

In order to be able to play a file, a number of xforms must be applied one
after the other, thus building a chain of xforms.  The chain is complete once a
goal format is reached.  A goal format is typically some type of PCM data that
can be sent to your audio hardware.

If you run the `xmms2d` daemon in a terminal, then you will see messages like
this:

    Successfully set up chain for 'file:///example.ogg' (456)
        containing gvfs:magic:vorbis:converter:segment:normalize

This is the chain of xforms that has been built up to handle playback of this
particular file.  In this case, `gvfs` has opened the file on disk, `magic`
has recognised the file format as Ogg Vorbis, which then allowed the `vorbis`
xform to decode it.  The `converter` xform then ran to convert the samples
into something more closely resembling what the audio hardware requires.
`segment` and `normalize` are effect plugins that are not required to play
the song but are added onto the chain through user preferences (see the _effect
xforms_ section below).

How priorities work
-------------------

Each xform takes in one or more different data formats, and produces a single
output format (which may be different depending on the input data.)  The input
data formats are assigned a priority, so that multiple xforms can handle the
same data format, with the priority controlling who gets to try it first.

In simple terms, watching the `xmms2d` console output will show which xform
chains and their priorities were examined in the process of playing a particular
file.  An example may look like this:

    Got full chain as 'gvfs:magic:adplug'
        (final priority 30 for 'audio/x-adplug-wlf')
    Got full chain as 'gvfs:libgamemusic_identify:libgamemusic'
        (final priority 40 for 'audio/x-libgamemusic-wlf-idsoftware-type1')
    Successfully set up chain ...

Here you can see that two xform chains were built, which means there are two
different plugins that can handle this file format.  The priorities of each
were compared to decide which one to actually use.  In this case, `libgamemusic`
was chosen as its priority was the largest number.

At the end of each message, the file format MIME type is shown.  This MIME type
is used to set the priority.  The priorities are maintained with the
`xmms2 server config` command:

    $ xmms2 server config | grep wlf
    adplug.priority.audio/x-adplug-wlf = 30
    libgamemusic.priority.audio/x-libgamemusic-wlf-idsoftware-type1 = 40

If the `adplug` xform should take higher priority, this can be changed by
setting the value higher:

    $ xmms2 server config adplug.priority.audio/x-adplug-wlf 50

You could of course achieve the same goal by lowering the priority of the other
xform as well.

How priorities really work
--------------------------

The above section covers all you need to know about using priorities to control
plugins.  However if you would like even more control, this section explains
the priority system in greater detail.

As mentioned above, each xform has a priority, not just the ones at the end of
the chain.  In most cases this intermediate priority does not matter, as all
plugins are eventually tried in an attempt to build a valid chain.

However as an example, the `gvfs` and `file` plugins can have their priorities
changed even though they don't handle specific file formats.  These are the
first two xforms used in most cases, as they both take `file://` URLs from the
playlist and produce binary streams that other plugins can decode.

If you run `xmms2d` in debug mode (`xmms2d -v`) you will see the process of
constructing xform chains in all its gory detail.  The first thing you will
notice is that by default, two chains are constructed for each file, with
either `gvfs` or `file` xforms at the root.

One of these will be tried first, and then the other.  Although it will have
no effect on the final outcome, we can change the priority of these to
illustrate the process.

    $ xmms2 server config | grep file://
    file.priority.application/x-url:file://* = 50
    gvfs.priority.application/x-url:file://* = 50

If one of these is made higher priority than the other, then it will always run
first.  However having both these xforms run is a bit inefficient, as we
construct two full xform chains that are otherwise identical, apart from one
starting with `gvfs` and the other starting with `file`:

    Got full chain as 'gvfs:magic:adplug'
        (final priority 30 for 'audio/x-adplug-wlf')
    Got full chain as 'gvfs:libgamemusic_identify:libgamemusic'
        (final priority 40 for 'audio/x-libgamemusic-wlf-idsoftware-type1')
    Got full chain as 'file:magic:adplug'
        (final priority 30 for 'audio/x-adplug-wlf')
    Got full chain as 'file:libgamemusic_identify:libgamemusic'
        (final priority 40 for 'audio/x-libgamemusic-wlf-idsoftware-type1')
    Successfully set up chain for 'file:///example.wlf' (567)
        containing gvfs:libgamemusic_identify:libgamemusic:segment:normalize

Here, the `adplug` and `libgamemusic` plugins had to be initialised twice (once
with data from `file` and again with data from `gvfs`).  One way to avoid this
redundancy is to completely disable one of the plugins, by setting its priority
to zero:

    $ xmms2 server config gvfs.priority.application/x-url:file://* 0

This certainly solves the problem, and now only the `file` plugin is used:

    Got full chain as 'file:magic:adplug' ...
    Got full chain as 'file:libgamemusic_identify:libgamemusic' ...
    Successfully set up chain ... containing file:...

However there is one drawback of this solution.  If, for some reason, the `file`
xform is unable to handle the data but the `gvfs` xform can, the song will be
unplayable because `gvfs` has been disabled.

A better solution is rather than disabling one plugin, to mark the other plugin
as preferred.  This is done by setting the priority above 100 (and under 200).
When an xform with a priority above 100 is encountered, and it can successfully
handle the data, no further xforms at that point in the chain will be tested.
However crucially, if the xform cannot handle the data, then the other xforms
will still be tried in case they do support the content.

This allows you to only use `file` and ignore `gvfs` most of the time, unless
`file` can't handle the data, in which case `gvfs` will then be tried.

    $ xmms2 server config file.priority.application/x-url:file://* 150

Setting a priority above 100 is rarely needed as the priority system can
normally work out an ideal solution, however in rare cases (like `gvfs` and
`file`), this can save some time moving between tracks by reducing the number
of xform chains that are built and subsequently discarded.

Ideally, plugins will set themselves up quickly enough that this does not
matter.  `gvfs` and `file` are just an exception because, being at the root of
the chain, they have a greater influence on the amount of work that is done to
produce a working chain.

Another thing to be aware of when using preferred priorities is that you can
inadvertently override the final chain priority.  Going back to the above
example:

    Got full chain as 'gvfs:magic:adplug'
        (final priority 30 for 'audio/x-adplug-wlf')
    Got full chain as 'gvfs:libgamemusic_identify:libgamemusic'
        (final priority 40 for 'audio/x-libgamemusic-wlf-idsoftware-type1')

Here, the `libgamemusic` chain will be chosen because its final priority is
higher than `adplug`.  However if you were to set the `magic` plugin as
preferred by making its priority larger than 100, then `adplug` will be chosen
to handle the song, even though it is lower priority.  This is because by
making `magic` preferred, `libgamemusic_identify` won't be used and so its
priority 40 chain will never exist.  This is another reason why preferred
priorities are best avoided unless there is no other option.

Effect xforms
-------------

Effect xforms are added onto the end of a complete chain and alter the audio in
some form or another.  The `normalize` xform for example, automatically adjusts
the song volume so that it is more or less constant regardless of the actual
recording level in the file.

Effects are applied in order, by setting configuration values `effect.order.X`
where `X` is a number, beginning at zero.

For example, to enable the `normalize` plugin:

    $ xmms2 server config effect.order.0 normalize

To see active effect plugins:

    $ xmms2 server config | grep effect
    effect.order.0 = normalize
    effect.order.1 =

There will always be an empty entry at the end, so there is a place for you to
add another effect plugin if you would like to.

To remove an effect plugin, set it to a blank value:

    $ xmms2 server config effect.order.0 ''
