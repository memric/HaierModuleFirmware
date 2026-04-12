# Edit following two lines to set component requirements (see docs)
set(COMPONENT_REQUIRES )
set(COMPONENT_PRIV_REQUIRES )

set(COMPONENT_SRCS "main.c" 
"ota.c"
"FWTP/fwtp.c")
set(COMPONENT_ADD_INCLUDEDIRS "")

set(EXTRA_COMPONENT_DIRS "../components")

register_component()