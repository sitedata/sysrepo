cmake_minimum_required(VERSION 2.8.12)

# correct RPATH usage on OS X
set(CMAKE_MACOSX_RPATH TRUE)

# list of all the examples
set(examples application_changes_example sr_set_item_example sr_get_items_example oper_data_example notif_subscribe_example
    notif_send_example rpc_send_example rpc_subscribe_example)

foreach(app_name IN LISTS examples)
    add_executable(${app_name} ${app_name}.c)
    target_link_libraries(${app_name} sysrepo yang)
endforeach(app_name)

# oven plugin
add_library(oven SHARED plugin/oven.c)
target_link_libraries(oven sysrepo)
