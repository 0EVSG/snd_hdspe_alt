# $FreeBSD$

.PATH: ${SRCTOP}/sys/dev/sound/pci

KMOD=	snd_hdspe_alt
SRCS=	device_if.h bus_if.h pci_if.h channel_if.h mixer_if.h
SRCS+=	hdspe.c hdspe-pcm.c hdspe.h

.include <bsd.kmod.mk>
