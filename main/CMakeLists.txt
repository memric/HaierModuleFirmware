# Edit following two lines to set component requirements (see docs)
set(COMPONENT_REQUIRES )
set(COMPONENT_PRIV_REQUIRES )

set(COMPONENT_SRCS "main.c" 
"telegram_bot.c"
"mqtt_service.c"
"ota.c"
"FWTP/fwtp.c"
"cJSON/cJSON.h")
set(COMPONENT_ADD_INCLUDEDIRS "")
set(COMPONENT_EMBED_TXTFILES "telegram_cert.pem")
set(COMPONENT_EMBED_TXTFILES "mqtt_cert.pem")

set(EXTRA_COMPONENT_DIRS "../components")

register_component()