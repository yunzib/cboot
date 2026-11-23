# ============================================================================
# FindMySQL.cmake — 最小 MySQL 客户端查找模块
#
# 只定义 ORM 驱动需要的头文件/客户端库：
#   MySQL_FOUND
#   MySQL_INCLUDE_DIR
#   MySQL_LIBRARY
#   MySQL::Client (UNKNOWN IMPORTED)
#
# 查找顺序：MySQL_ROOT / 环境 MYSQL_ROOT → 常见前缀 → 系统路径。
# 不处理服务端、mysqld 或嵌入式库。
# ============================================================================

find_path(MySQL_INCLUDE_DIR
    NAMES mysql.h
    HINTS "${MySQL_ROOT}" "$ENV{MYSQL_ROOT}" "${MYSQL_ROOT}"
    PATH_SUFFIXES include)

find_library(MySQL_LIBRARY
    NAMES libmysql mysqlclient
    HINTS "${MySQL_ROOT}" "$ENV{MYSQL_ROOT}" "${MYSQL_ROOT}"
    PATH_SUFFIXES lib lib64)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(MySQL
    REQUIRED_VARS MySQL_INCLUDE_DIR MySQL_LIBRARY)

if(MySQL_FOUND AND NOT TARGET MySQL::Client)
    add_library(MySQL::Client UNKNOWN IMPORTED)
    set_target_properties(MySQL::Client PROPERTIES
        IMPORTED_LOCATION "${MySQL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${MySQL_INCLUDE_DIR}")
endif()

mark_as_advanced(MySQL_INCLUDE_DIR MySQL_LIBRARY)
