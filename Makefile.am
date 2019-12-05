# Makefile.am -- Process this file with automake to produce Makefile.in
bin_PROGRAMS = contents_delivery_manager

contents_delivery_manager_SOURCES = source/manager.c common/delivery.c common/delivery.h common/utils.h common/sha256.c common/sha256-compress.c common/write-be32.c common/sha2.h common/nettle-types.h common/sha2-internal.h common/macros.h common/nettle-write.h

contents_delivery_manager_LDADD = @NET_LIBS@

EXTRA_DIST = autogen.sh
