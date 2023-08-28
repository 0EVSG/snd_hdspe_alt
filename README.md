# snd_hdspe_alt

Alternative FreeBSD kernel module for RME AIO and RayDAT PCIe sound cards.

This code was forked from FreeBSD base source to add the following
features:

 * Set the preferred clock source via sysctl knob. Choose between clock master
   mode (internal) and one of the digital inputs for autosync mode.
 * Display the currenly effective clock source in a sysctl.
 * List the signal lock and sync status for clock sources (digital inputs).


## Usage

Output of `sysctl dev.hdspe` may look like this:
```
dev.hdspe.0.clock_list: internal,word,aes,spdif,adat1,adat2,adat3,adat4,tco,sync_in
dev.hdspe.0.clock_preference: internal
dev.hdspe.0.clock_source: internal
dev.hdspe.0.sync_status: word(none),aes(none),spdif(none),adat1(none),adat2(none),adat3(sync),adat4(none),tco(none)
dev.hdspe.0.%parent: pci10
dev.hdspe.0.%pnpinfo: vendor=0x10ee device=0x3fc6 subvendor=0x0000 subdevice=0x0000 class=0x040100
dev.hdspe.0.%location: slot=0 function=0 dbsf=pci0:96:0:0 handle=\_SB_.PCI1.IOH7.SLT7
dev.hdspe.0.%driver: hdspe
dev.hdspe.0.%desc: RME HDSPe RayDAT
dev.hdspe.%parent:
```

Supported clock sources are listed in `clock_list`. To select e.g. SPDIF input
as the preferred clock source, set the `clock_preference`:
```
# sysctl dev.hdspe.0.clock_preference=spdif
```

The currently effective clock source is shown in `clock_source`. For each
digital input, the `sync_status` shows `none` for no signal, `lock` for a valid
signal, and `sync` for a completely synchronized source (required for recording
digital inputs).
