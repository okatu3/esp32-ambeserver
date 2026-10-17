idf_component_register(
    SRCS main.c dv3000_protocol.c ambe_codec.c transport_uart.c
    INCLUDE_DIRS "."
    REQUIRES mbelib_neo driver
)
