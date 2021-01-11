TOPDIR = $(CURDIR)

ARCH	= $(shell uname -m)

CC	= gcc
AR	= ar
ARFLAGS	= rcs
INSTALL	= install

# Target name for per target flags
basetarget = $(basename $@)

ifeq ($(ARCH),x86_64)
CFLAGS	+= -m64
endif
ifeq ($(ARCH),i686)
CFLAGS	+= -m32
endif

CFLAGS	+= -std=gnu99 -Wall -g -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
CFLAGS	+= -I$(TOPDIR) -DTUX3_BUILD=1
# gcc warning options
CFLAGS	+= -Wall -Wextra -Werror
CFLAGS	+= -Wundef -Wstrict-prototypes -Werror-implicit-function-declaration
CFLAGS	+= -Wno-unused-parameter -Wno-sign-compare
CFLAGS	+= -Wno-missing-field-initializers
#CFLAGS += -Wp,-Wunused-macros

# Debugging
#
# backtrace() would need this to see all symbols
CFLAGS	+= -rdynamic
# buffer debug
CFLAGS	+= -DBUFFER_PARANOIA_DEBUG=1
# cursor debug (this is defined in tux3.h)
#CFLAGS	+= -DCURSOR_DEBUG=1
# use LOCK_DEBUG
CFLAGS	+= -DLOCK_DEBUG=1
# use SMALL_PAGE_SIZE_DEBUG
CFLAGS	+= -DSMALL_PAGE_SIZE_DEBUG=1
# alignment checker
#ALIGN_CHECK = 1
ifeq ($(ALIGN_CHECK),1)
CFLAGS	+= -DALIGN_CHECK=1
LIBS += -ldl
# prevent unaligned error
NEED_MEMMOVE_UNALIGNED = 1
endif
# use refcount_t debugging
CFLAGS	+= -DCONFIG_REFCOUNT_FULL=1

# gcc checker (_FORTIFY_SOURCE requires optimize option >= 1)
# Rethink about this when optimization become default
#CFLAGS	+= -O1 -D_FORTIFY_SOURCE=2
# gcc sanitizers
ifeq ($(ASAN),1)
CFLAGS	+= -fsanitize=address -fsanitize=leak
endif
ifeq ($(UBSAN),1)
CFLAGS	+= -fsanitize=undefined
# prevent unaligned error
NEED_MEMMOVE_UNALIGNED = 1
endif
#CFLAGS += -fsanitize=thread

ifeq ($(NEED_MEMMOVE_UNALIGNED),1)
# To use memmove() for unaligned access
CFLAGS	+= -DCONFIG_MEMMOVE_UNALIGNED_ACCESS=1
endif

# gcov
ifneq ($(GCOV),)
CFLAGS	+= --coverage
endif
# userland can't use async, so use TUX3_FLUSHER_SYNC
CFLAGS	+= -DTUX3_FLUSHER_SYNC
# Per target CFLAGS
CFLAGS	+= $(CFLAGS_$(basetarget).o)
# user flags
CFLAGS	+= $(UCFLAGS)

# Per target LDFLAGS/LIBS
LDFLAGS	+= $(LDFLAGS_$@)
LIBS	+= $(LIBS_$@)

AFLAGS	= $(CFLAGS) -D__ASSEMBLY__
# .S default is not noexecstack
AFLAGS	+= -Wa,--noexecstack
AFLAGS	+= $(AFLAGS_$(basetarget).o)

CHECKER	   = sparse
CHECKFLAGS = -gcc-base-dir $(shell $(CC) -print-file-name=)
# Add multiarch include if need (workaround for debian's multiarch)
CHECKFLAGS += $(shell if gcc -print-multiarch > /dev/null 2>&1; then \
		echo "-isystem /usr/include/`gcc -print-multiarch`"; fi)
CHECKFLAGS += -D__CHECKER__
CHECKFLAGS += -Wsparse-all
CHECKFLAGS += -Wno-unknown-attribute -Wno-transparent-union -Wno-return-void
# suppress potentially expensive ptr sub
CHECKFLAGS += -Wno-ptr-subtraction-blows
# suppress non-static warning
#CHECKFLAGS += -Wno-decl
# suppress mixed declaration
CHECKFLAGS += -Wno-declaration-after-statement
# too many false positive (sparse bug?)
CHECKFLAGS += -Wno-pointer-arith -Wno-bitwise-pointer -Wno-constexpr-not-const

TESTDIR	= $(TOPDIR)
OWNER	= root
GROUP	= root

# To use clock_gettime(), have to link librt if glibc is 2.16 or before
#LIBS += -lrt

# check lcov option
lcov-opt-if = $(shell						\
	if lcov --help 2> /dev/null | grep -q -- "$(1)"; then	\
		echo "$(2)";					\
	else							\
		echo "$(3)";					\
	fi)
# lcov flags
TUX3_DIR = $(abspath $(TOPDIR)/../)
GCOV_DIR = gcov
LCOV_FLAGS = --capture
GENHTML_FLAGS = --prefix $(TUX3_DIR) --show-details --legend
# lcov-1.10 or above, branch coverage is off by default, enable it
LCOV_FLAGS += $(call lcov-opt-if,--rc,--rc lcov_branch_coverage=1)
GENHTML_FLAGS += $(call lcov-opt-if,--rc,--branch-coverage)

DISTDIR	=
PREFIX	= /usr/local
SBINDIR	= $(PREFIX)/sbin
LIBEXECDIR = $(PREFIX)/libexec/tux3

# binary or shared library
all-bin		:=
# static library
all-static	:=

# libraries
USER_OBJS	= commit.o current_task.o dir.o diskio.o filemap.o inode.o \
	namei.o options.o super.o utility.o writeback.o
ifeq ($(ALIGN_CHECK),1)
USER_OBJS	+= aligncheck.o
endif
KERN_OBJS	= ../balloc.o ../btree.o ../dleaf.o ../iattr.o ../ileaf.o \
	../log.o ../orphan.o ../policy.o ../replay.o ../xattr.o
TEST_LIB_OBJS	= tests/bench.o tests/test.o

all-static	+= libtux3.a
libtux3.a-objs	= $(USER_OBJS) $(KERN_OBJS) $(TEST_LIB_OBJS)
# use -include tux3user.h to compile ../* without any change
$(KERN_OBJS): CFLAGS += -include tux3user.h

all-static	+= libklib/libklib.a
libklib/libklib.a-objs = libklib/find_bit.o libklib/fs.o libklib/fs_types.o \
	libklib/list_sort.o libklib/mm-util.o libklib/parser.o \
	libklib/refcount.o libklib/seq_file.o libklib/slab.o libklib/string.o \
	libklib/time.o libklib/uidgid.o

# binaries
all-bin		+= tux3
tux3-objs	= tux3.o $(all-static)

ifeq ($(shell pkg-config fuse3 && echo found), found)
	all-bin += tux3fuse
	tux3fuse-objs = tux3fuse.o $(all-static)
	CFLAGS_tux3fuse.o = $(shell pkg-config --cflags fuse3)
	LIBS_tux3fuse = $(shell pkg-config --libs fuse3)
endif

# tests/*
TEST_BIN	= tests/libklib \
	tests/balloc tests/bdict_fixed tests/btree tests/buffer \
	tests/commit tests/dir tests/dleaf tests/fault tests/filemap \
	tests/iattr tests/ileaf tests/inode tests/link tests/log tests/xattr

# add libm to all tests/* if needed
define add_libm_to_bin
LIBS_$(1) += -Wl,--push-state,--as-needed -lm -Wl,--pop-state
endef
$(foreach bin,$(TEST_BIN),$(eval $(call add_libm_to_bin,$(bin))))

LIBS_tests/fault	+= -ldl

# make define for binary that compiled from one source file
define onefile_bin_template =
all-bin += $(1)
$(1)-objs = $(1).o $$(all-static)
endef
$(foreach bin,$(TEST_BIN),$(eval $(call onefile_bin_template,$(bin))))


# objects dependency
ALL_TARGETS	:= $(all-bin) $(all-static)
ALL_OBJS	:=

.PHONY: all tests
all: $(ALL_TARGETS)

define dep_template =
$(1): $$($(1)-objs)
ALL_OBJS += $$(filter-out $$(all-static),$$($(1)-objs))
endef
$(foreach bin,$(ALL_TARGETS),$(eval $(call dep_template,$(bin))))

# directories includes objects
OBJS_DIRS	:= $(sort $(dir $(ALL_OBJS)))

# dependency generation
DEPDIR    := .deps
DEP_FILES := $(foreach f,$(ALL_OBJS),$(dir $f)$(DEPDIR)/$(notdir $f).d)
DEP_DIRS  := $(addsuffix $(DEPDIR),$(OBJS_DIRS))
MISSING_DEP_DIRS := $(filter-out $(wildcard $(DEP_DIRS)),$(DEP_DIRS))

$(DEP_DIRS):
	$(Q)mkdir -p $@

DEP_FILE = $(dir $@)$(DEPDIR)/$(notdir $@).d
DEP_ARGS = -MF $(DEP_FILE) -MP -MMD
# Take advantage of gcc's on-the-fly dependency generation
# See <http://gcc.gnu.org/gcc-3.0/features.html>.
DEP_FILES_PRESENT := $(wildcard $(DEP_FILES))
-include $(DEP_FILES_PRESENT)

# verbose or quiet
ifeq ($(V),1)
Q :=
cmd =	$(2)
else
Q :=	@
cmd =	@echo "  "$(1); $(2)
endif

# run checker or nothing
C ?= $(CHECK)
ifeq ($(C),1)
chk =	$(call cmd,$(1),$(2))
else
chk =
endif

# binary or shared library rules
$(all-bin):
	$(call cmd,"LINK      $@",\
		$(CC) $(CFLAGS) $(LDFLAGS) $^ -o $@ $(LIBS))

%.o: %.S $(MISSING_DEP_DIRS)
	$(call cmd,"CC        $@",\
		$(CC) $(DEP_ARGS) $(AFLAGS) -c -o $@ $<)

%.o: %.c $(MISSING_DEP_DIRS)
	$(call cmd,"CC        $@",\
		$(CC) $(DEP_ARGS) $(CFLAGS) -c -o $@ $<)
	$(call chk,"CHECK     $<",\
		$(CHECKER) $(CHECKFLAGS) $(CFLAGS) -c $<)

# static library rules
$(all-static):
	$(call cmd,"AR        $@",\
		rm -f $@ && $(AR) $(ARFLAGS) $@ $^)

.PHONY: clean
clean:
	$(call cmd,"CLEAN")
	$(Q)rm -f $(ALL_TARGETS) $(ALL_OBJS)
	$(Q)rm -f a.out $(TESTDIR)/testdev
	$(Q)rm -f $(addsuffix *.gcda,$(OBJS_DIRS))
	$(Q)rm -f $(addsuffix *.gcno,$(OBJS_DIRS))
	$(Q)$(MAKE) -C tests clean

distclean: clean
	$(call cmd,"DISTCLEAN")
	$(Q)rm -f $(addsuffix *.orig,$(OBJS_DIRS))
	$(Q)rm -rf $(DEP_DIRS)
	$(Q)rm -rf $(GCOV_DIR)

install: install-bin install-test

install-bin: $(TUX3_BIN) $(FUSE_BIN)
	$(INSTALL) -c -o $(OWNER) -g $(GROUP) -m 755 -d $(DISTDIR)$(SBINDIR)
	$(INSTALL) -c -o $(OWNER) -g $(GROUP) -m 755 $(TUX3_BIN) $(FUSE_BIN) $(DISTDIR)$(SBINDIR)

install-test: install-bin $(TEST_BIN)
	$(INSTALL) -c -o $(OWNER) -g $(GROUP) -m 755 -d $(DISTDIR)$(LIBEXECDIR)
	$(INSTALL) -c -o $(OWNER) -g $(GROUP) -m 755 $(TEST_BIN) $(DISTDIR)$(LIBEXECDIR)

# tests rules
tests: $(TEST_BIN)
	$(MAKE) -C tests

# Use lcov to summarize gcov data
coverage:
	# NOTE: need to compile with GCOV=1
	[ -d $(GCOV_DIR) ] || mkdir $(GCOV_DIR)
	lcov $(LCOV_FLAGS) -b $(TOPDIR) -d $(TUX3_DIR) -o $(GCOV_DIR)/tux3.info
	genhtml $(GENHTML_FLAGS) -o $(GCOV_DIR) $(GCOV_DIR)/tux3.info

makefs mkfs: tux3
	dd if=/dev/zero of=$(TESTDIR)/testdev bs=1 count=1 seek=1M
	./tux3 mkfs $(TESTDIR)/testdev
	mkdir -p $(TESTDIR)/test
	if [[ ! -f /etc/fuse.conf ]]; then sudo sh -c "echo user_allow_other >/etc/fuse.conf"; fi

testfs testfuse: makefs
	./tux3fuse $(TESTDIR)/testdev $(TESTDIR)/test -o allow_other

debug defuse: tux3fuse
	sudo ./tux3fuse $(TESTDIR)/testdev $(TESTDIR)/test -o allow_other -f

untest:
	sudo umount $(TESTDIR)/test || true
	rmdir $(TESTDIR)/test

unbork:
	sudo umount -l $(TESTDIR)/test
