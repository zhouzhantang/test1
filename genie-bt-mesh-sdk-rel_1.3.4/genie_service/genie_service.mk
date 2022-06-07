NAME := genie_service

$(NAME)_MBINS_TYPE := kernel
$(NAME)_SUMMARY := Genie Service

GLOBAL_CFLAGS += -DSYSINFO_BUILD_TIME=\"$(CURRENT_TIME)\"

$(NAME)_COMPONENTS += bluetooth.bt_mesh yloop rhino.fs.kv cli bluetooth.bt_host crc

$(NAME)_INCLUDES += ../network/bluetooth/bt_mesh/inc \
					../network/bluetooth/bt_mesh/inc/api \
					../network/bluetooth/bt_mesh/inc/port \
					../network/bluetooth/bt_mesh/mesh_model \
					../network/bluetooth/bt_mesh/vendor_model \
					../network/bluetooth/bt_mesh/inc \
					../genie_service/core/inc \
					../genie_service/sal/inc \
					../utility/crc \
					../genie_service

$(NAME)_SOURCES  += genie_service.c \
					core/src/genie_cli.c \
					core/src/genie_reset.c \
					core/src/genie_event.c \
					core/src/genie_storage.c \
					core/src/genie_triple.c \
					core/src/genie_vendor_model.c \
					core/src/genie_transport.c \
					core/src/genie_crypto.c \
					core/src/genie_version.c \
					core/src/genie_provision.c \
					core/src/genie_mesh.c

ifeq ($(GENIE_GPIO_UART),1)
GLOBAL_DEFINES += CONFIG_GPIO_UART
endif

ifeq ($(GENIE_MESH_NO_AUTO_REPLY),1)
GLOBAL_DEFINES += CONFIG_GENIE_MESH_NO_AUTO_REPLY
endif

ifeq ($(GENIE_FACTORY_TEST),1)
GLOBAL_DEFINES += CONFIG_GENIE_FACTORY_TEST
endif

ifeq ($(GENIE_MESH_AT_CMD),1)
GLOBAL_DEFINES += CONFIG_GENIE_MESH_AT_CMD
$(NAME)_SOURCES += core/src/genie_at.c
$(NAME)_SOURCES += sal/src/genie_sal_uart.c
endif

ifeq ($(GENIE_MESH_BINARY_CMD),1)
GLOBAL_DEFINES += CONIFG_GENIE_MESH_BINARY_CMD
$(NAME)_SOURCES += core/src/genie_bin_cmds.c
$(NAME)_SOURCES += sal/src/genie_sal_uart.c
endif

ifeq ($(GENIE_MESH_USER_CMD),1)
GLOBAL_DEFINES += CONIFG_GENIE_MESH_USER_CMD
endif

ifeq ($(genie_time_config),1)
GLOBAL_DEFINES += MESH_MODEL_VENDOR_TIMER
$(NAME)_SOURCES += core/src/genie_time.c
endif

ifeq ($(GENIE_MESH_GLP),1)
ifeq ($(genie_lpm_config),0)
$(error "You should config genie_lpm_config=1 when you use GLP")
endif
GLOBAL_DEFINES += CONFIG_GENIE_MESH_GLP
endif

ifeq ($(genie_lpm_config),1)
$(NAME)_SOURCES += core/src/genie_lpm.c
$(NAME)_SOURCES += sal/src/genie_sal_lpm.c
GLOBAL_DEFINES += CONFIG_PM_SLEEP
endif

$(NAME)_SOURCES += sal/src/genie_sal_ble.c sal/src/genie_sal_provision.c

bt_host_smp_config = 1
bt_host_tinycrypt_config = 0

# Host configurations
GLOBAL_DEFINES += CONFIG_BLUETOOTH
GLOBAL_DEFINES += CONFIG_BT_CONN
#GLOBAL_DEFINES += CONFIG_BT_CENTRAL
GLOBAL_DEFINES += CONFIG_BT_PERIPHERAL

# Feature configurations
#GLOBAL_DEFINES += GENIE_OLD_AUTH
#GLOBAL_DEFINES += GENIE_ULTRA_PROV

GLOBAL_DEFINES += CONFIG_BT_MESH
GLOBAL_DEFINES += CONFIG_GENIE_RESET_BY_REPEAT

####### ota config #######
ifeq ($(genie_ota_config),1)
GLOBAL_DEFINES += CONFIG_GENIE_OTA
$(NAME)_SOURCES  += core/src/genie_ais.c
$(NAME)_SOURCES  += core/src/genie_ota.c
ifeq ($(HOST_ARCH), tc32)
GLOBAL_DEFINES += CONFIG_GENIE_OTA_PINGPONG
endif
endif

####### model config #######

#check config
ifeq ($(MESH_MODEL_CTL_SRV),1)
MESH_MODEL_GEN_ONOFF_SRV = 1
MESH_MODEL_LIGHTNESS_SRV = 1
endif

ifeq ($(MESH_MODEL_LIGHTNESS_SRV),1)
MESH_MODEL_GEN_ONOFF_SRV = 1
endif

ifeq ($(MESH_MODEL_ENABLE_TRANSITION),1)
GLOBAL_DEFINES += CONFIG_MESH_MODEL_TRANS
endif

ifeq ($(MESH_MODEL_GEN_ONOFF_SRV),1)
GLOBAL_DEFINES += CONFIG_MESH_MODEL_GEN_ONOFF_SRV
$(NAME)_SOURCES += core/src/sig_models/sig_model_onoff_srv.c
endif

ifeq ($(MESH_MODEL_LIGHTNESS_SRV),1)
GLOBAL_DEFINES += CONFIG_MESH_MODEL_LIGHTNESS_SRV
GLOBAL_DEFINES += CONFIG_MESH_MODEL_LIGHT_CMD
$(NAME)_SOURCES += core/src/sig_models/sig_model_lightness_srv.c
endif

ifeq ($(MESH_MODEL_CTL_SRV),1)
GLOBAL_DEFINES += CONFIG_MESH_MODEL_CTL_SRV
GLOBAL_DEFINES += CONFIG_MESH_MODEL_CTL_SETUP_SRV
#GLOBAL_DEFINES += CONFIG_MESH_MODEL_CTL_TEMPERATURE_SRV
$(NAME)_SOURCES += core/src/sig_models/sig_model_light_ctl_srv.c
endif

ifeq ($(MESH_MODEL_SCENE_SRV),1)
GLOBAL_DEFINES += CONFIG_MESH_MODEL_SCENE_SRV
$(NAME)_SOURCES += core/src/sig_models/sig_model_scene_srv.c
endif

$(NAME)_SOURCES += core/src/sig_models/sig_model_bind_ops.c
$(NAME)_SOURCES += core/src/sig_models/sig_model_transition.c
$(NAME)_SOURCES += core/src/sig_models/sig_model_event.c
