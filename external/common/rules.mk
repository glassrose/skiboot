ARCH = $(shell $(GET_ARCH) "$(CROSS_COMPILE)")

ifeq ($(ARCH),ARCH_ARM)
arch = arm
ARCH_OBJS = common-arch_flash_common.o common-arch_flash_arm.o common-ast-sf-ctrl.o
else
ifeq ($(ARCH),ARCH_POWERPC)
arch = powerpc
ARCH_OBJS = common-arch_flash_common.o common-arch_flash_powerpc.o
else
ifeq ($(ARCH),ARCH_X86)
arch = x86
ARCH_OBJS = common-arch_flash_common.o common-arch_flash_x86.o
else
$(error Unsupported architecture $(ARCH))
endif
endif
endif


# Arch links are like this so we can have dependencies work (so that we don't
# run the rule when the links exist), pretty build output (knowing the target
# name) and a list of the files so we can clean them up.
ARCH_LINKS = common/ast-sf-ctrl.c common/ast.h common/io.h

arch_links: $(ARCH_LINKS)
common/ast.h : ../../include/ast.h | common
	$(Q_LN)ln -sf ../../include/ast.h common/ast.h

common/io.h : ../common/arch_flash_$(arch)_io.h | common
	$(Q_LN)ln -sf arch_flash_$(arch)_io.h common/io.h

common/ast-sf-ctrl.c : ../../hw/ast-bmc/ast-sf-ctrl.c | common
	$(Q_LN)ln -sf ../../hw/ast-bmc/ast-sf-ctrl.c common/ast-sf-ctrl.c

.PHONY: arch_clean
arch_clean:
	rm -rf $(ARCH_OBJS) $(ARCH_LINKS)

$(ARCH_OBJS): common-%.o: common/%.c
	$(Q_CC)$(CROSS_COMPILE)gcc $(CFLAGS) -c $< -o $@

common-arch_flash.o: $(ARCH_OBJS)
	$(Q_LD)$(CROSS_COMPILE)ld $(LDFLAGS) -r $(ARCH_OBJS) -o $@

