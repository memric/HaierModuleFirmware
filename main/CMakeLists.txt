# Edit following two lines to set component requirements (see docs)
set(COMPONENT_REQUIRES )
set(COMPONENT_PRIV_REQUIRES )

set(COMPONENT_SRCS "main.c" 
"telegram_bot.c"
"ota.c"
"FWTP/fwtp.c")
set(COMPONENT_ADD_INCLUDEDIRS "")
set(COMPONENT_EMBED_TXTFILES "telegram_cert.pem")

set(EXTRA_COMPONENT_DIRS "../components")

register_component()