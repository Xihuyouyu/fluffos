if(${PACKAGE_ERRCODE})
    add_library(package_errcode STATIC
            "errcode.cc"
            "errcode.h"
            )
endif()