# snd_hdspe_alt

Alternative FreeBSD kernel module for RME AIO and RayDAT PCIe sound cards.

This code was forked from FreeBSD base source to add the following
features and changes:

 * Create one PCM device per physical port, don't split ADAT ports into
   multiple stereo PCM devices.
 * Create one unified PCM device for all physical ports combined, if needed.
 * Set the preferred clock source via sysctl knob. Choose between clock master
   mode (internal) and one of the digital inputs for autosync mode.
 * Display the currenly effective clock source in a sysctl.
 * List the signal lock and sync status for clock sources (digital inputs).
 * Set fixed period and sample rate to circumvent issues with PCM device setup.

To load this module, disable the original `snd_hdspe` and `kldload snd_hdspe_alt`.
For permanent use add the following line to `/boot/loader.conf`:
```
snd_hdspe_alt_load="YES"
```


## Device Layout

The FreeBSD snd_hdspe driver splits the physical ADAT ports into four 2-channel
PCM devices. This means the four ADAT ports of my RayDAT card result in 16(!)
different stereo PCM devices. Not only is this cumbersome to use with any multi
channel audio software like JACK, the mapping to physical ports also differs
between single speed, double speed and quad speed sample rates.

In contrast, the PCM device layout of snd_hdspe_alt is one per physical port:
```
pcm0: <HDSPe RayDAT [aes]> (play/rec)
pcm1: <HDSPe RayDAT [s/pdif]> (play/rec)
pcm2: <HDSPe RayDAT [adat1]> (play/rec)
pcm3: <HDSPe RayDAT [adat2]> (play/rec)
pcm4: <HDSPe RayDAT [adat3]> (play/rec)
pcm5: <HDSPe RayDAT [adat4]> (play/rec)
```

The ADAT devices can be opened with 2, 4 or 8 channels width, regardless of
the sample rate. Please note that some channels will be left silent if the
PCM channels do not match the ADAT channel width at given sample rate.

There's also the option to create one unified PCM device. This is a tunable
that has to be set before the kernel module is loaded. In `/boot/loader.conf`:
```
hw.hdspe.unified_pcm="1"
```

This combines all physical ports in one PCM device with lots of channels, and
is mostly useful with multi channel audio software. For RayDAT cards the channel
count is 36 at single speed (48kHz), 20 at double speed (96kHz) and 12 at quad
speed (192kHz). The channel count of AIO cards is 14 / 10 / 8 for playback, and
12 / 8 / 6 for recording, respectively.


## Clock Source

Let's have a look at the clock source related output of `sysctl dev.hdspe`:
```
dev.hdspe.0.clock_list: internal,word,aes,spdif,adat1,adat2,adat3,adat4,tco,sync_in
dev.hdspe.0.clock_preference: internal
dev.hdspe.0.clock_source: internal
dev.hdspe.0.sync_status: word(none),aes(none),spdif(none),adat1(none),adat2(none),adat3(sync),adat4(none),tco(none)
```

Supported clock sources are listed in `clock_list`. The naming follows RME
terminology, where `internal` defines the HDSPe card as clock master. Other
choices for `clock_preference` act in autosync mode. The following example
selects SPDIF input as preferred clock source, but will sync to whatever is
available if there is no SPDIF signal.
```
# sysctl dev.hdspe.0.clock_preference=spdif
```

The currently effective clock source is shown in `clock_source`. For each
digital input, the `sync_status` shows `none` for no signal, `lock` for a valid
signal, and `sync` for a completely synchronized source (required for recording
digital inputs).


## Period and Sample Rate

Period is the number of samples between interrupts, and as such determines the
interrupt rate at a given sample rate. Faster interrupt rates will result in a
lower latency, at the expense of more CPU wakeups. Unfortunately, the FreeBSD
PCM latency profiles try to setup different periods for playback and recording.
To have a well-defined period and consistent buffer configurations, this module
provides a `period` sysctl knob:
```
# sysctl dev.hdspe.0.period=64
```

Another problem is that currently PCM channel configurations are not negotiated
with the driver if there's more than 8 channels. Thus the unified PCM devices
always just select the first configuration offered by the driver. The
`sample_rate` sysctl knob both sets a fixed sample rate and makes the
corresponding channel configuration the first one being offered to the PCM
device.
```
# sysctl dev.hdspe.0.sample_rate=96000
```
