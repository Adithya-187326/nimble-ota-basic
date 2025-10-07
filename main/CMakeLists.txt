set(srcs "main.c"
         "ota_svr.c")

idf_component_register(SRCS "${srcs}"
                       INCLUDE_DIRS ".")
