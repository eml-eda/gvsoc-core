export VP_ROOT_DIR = $(CURDIR)

define declareInstallFile

$(INSTALL_DIR)/$(1): $(1)
	install -D $(1) $$@

TARGETS += $(INSTALL_DIR)/$(1)

endef

INSTALL_FILES += bin/pulp-pc-info
$(foreach file, $(INSTALL_FILES), $(eval $(call declareInstallFile,$(file))))



clean:
	make -C engine clean
	make -C launcher clean
	make -C models clean

build: $(TARGETS)
	install -D vp_models.mk $(INSTALL_DIR)/rules/vp_models.mk
ifdef VP_USE_SYSTEMC_DRAMSYS
	mkdir -p models/memory/dram.sys/build && cd models/memory/dram.sys/build && qmake ../DRAMSys/DRAMSys.pro && make
endif
	make -C engine build
	make -C launcher build
	make -C models props
	make -C models build

checkout:
	git submodule update --init
