# Makefile.am -- Process this file with automake to produce Makefile.in
bin_PROGRAMS = contents_delivery_manager

contents_delivery_manager_SOURCES = source/manager.c common/delivery.c common/delivery.h common/utils.h common/nettle/sha256.c common/nettle/sha256-compress.c common/nettle/write-be32.c common/nettle/sha2.h common/nettle/nettle-types.h common/nettle/sha2-internal.h common/nettle/macros.h common/nettle/nettle-write.h

contents_delivery_manager_LDADD = @NET_LIBS@

EXTRA_DIST = autogen.sh
