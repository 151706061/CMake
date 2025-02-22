set(expect [[
# file\(GENERATE\) produced:
iface1 CUSTOM_A: ''
iface1 INTERFACE_CUSTOM_A: 'CUSTOM_A_IFACE1;CUSTOM_A_TARGET_NAME_IFACE1'
iface2 CUSTOM_A: ''
iface2 INTERFACE_CUSTOM_A: 'CUSTOM_A_IFACE2;CUSTOM_A_TARGET_TYPE_INTERFACE_LIBRARY;CUSTOM_A_IFACE1;CUSTOM_A_TARGET_NAME_IFACE2'
static1 CUSTOM_A: 'CUSTOM_A_STATIC1;CUSTOM_A_IFACE2;CUSTOM_A_TARGET_TYPE_STATIC_LIBRARY;CUSTOM_A_IFACE1;CUSTOM_A_TARGET_NAME_STATIC1'
static1 INTERFACE_CUSTOM_A: 'CUSTOM_A_STATIC1_IFACE'
static1 CUSTOM_B: 'CUSTOM_B_STATIC1;CUSTOM_B_IFACE1'
static1 INTERFACE_CUSTOM_B: 'CUSTOM_B_STATIC1_IFACE'
static1 CUSTOM_U: 'CUSTOM_U_STATIC1;CUSTOM_U_IFACE2;CUSTOM_U_TARGET_TYPE_STATIC_LIBRARY;CUSTOM_U_IFACE1;CUSTOM_U_TARGET_NAME_STATIC1'
static1 INTERFACE_CUSTOM_U: 'CUSTOM_U_STATIC1_IFACE;CUSTOM_U_IFACE2;CUSTOM_U_TARGET_TYPE_STATIC_LIBRARY;CUSTOM_U_IFACE1;CUSTOM_U_TARGET_NAME_STATIC1'
static1 CUSTOM_V: 'CUSTOM_V_STATIC1;CUSTOM_V_IFACE1'
static1 INTERFACE_CUSTOM_V: 'CUSTOM_V_STATIC1_IFACE;CUSTOM_V_IFACE1'
object1 CUSTOM_A: 'CUSTOM_A_OBJECT1;CUSTOM_A_IFACE2;CUSTOM_A_TARGET_TYPE_OBJECT_LIBRARY;CUSTOM_A_IFACE1;CUSTOM_A_TARGET_NAME_OBJECT1'
object1 INTERFACE_CUSTOM_A: 'CUSTOM_A_OBJECT1_IFACE'
object1 CUSTOM_C: 'CUSTOM_C_OBJECT1;CUSTOM_C_IFACE1'
object1 INTERFACE_CUSTOM_C: 'CUSTOM_C_OBJECT1_IFACE'
object1 CUSTOM_U: 'CUSTOM_U_OBJECT1;CUSTOM_U_IFACE2;CUSTOM_U_TARGET_TYPE_OBJECT_LIBRARY;CUSTOM_U_IFACE1;CUSTOM_U_TARGET_NAME_OBJECT1'
object1 INTERFACE_CUSTOM_U: 'CUSTOM_U_OBJECT1_IFACE;CUSTOM_U_IFACE2;CUSTOM_U_TARGET_TYPE_OBJECT_LIBRARY;CUSTOM_U_IFACE1;CUSTOM_U_TARGET_NAME_OBJECT1'
object1 CUSTOM_W: 'CUSTOM_W_OBJECT1;CUSTOM_W_IFACE1'
object1 INTERFACE_CUSTOM_W: 'CUSTOM_W_OBJECT1_IFACE;CUSTOM_W_IFACE1'
main CUSTOM_A: 'CUSTOM_A_MAIN'
main INTERFACE_CUSTOM_A: ''
main CUSTOM_B: 'CUSTOM_B_MAIN;CUSTOM_B_STATIC1_IFACE'
main INTERFACE_CUSTOM_B: ''
main CUSTOM_C: 'CUSTOM_C_MAIN;CUSTOM_C_OBJECT1_IFACE'
main INTERFACE_CUSTOM_C: ''
main CUSTOM_U: 'CUSTOM_U_MAIN;CUSTOM_U_STATIC1_IFACE;CUSTOM_U_IFACE2;CUSTOM_U_TARGET_TYPE_EXECUTABLE;CUSTOM_U_IFACE1;CUSTOM_U_TARGET_NAME_CUSTOMTRANSITIVEPROPERTIES;CUSTOM_U_OBJECT1_IFACE'
main INTERFACE_CUSTOM_U: ''
main CUSTOM_V: 'CUSTOM_V_MAIN;CUSTOM_V_STATIC1_IFACE;CUSTOM_V_IFACE1'
main INTERFACE_CUSTOM_V: ''
main CUSTOM_W: 'CUSTOM_W_MAIN;CUSTOM_W_IFACE1;CUSTOM_W_OBJECT1_IFACE'
main INTERFACE_CUSTOM_W: ''

iface1 LINK_LIBRARIES: ''
iface1 INTERFACE_LINK_LIBRARIES: ''
iface2 LINK_LIBRARIES: ''
iface2 INTERFACE_LINK_LIBRARIES: 'iface1'
static1 LINK_LIBRARIES: 'iface2'
static1 INTERFACE_LINK_LIBRARIES: '\$<LINK_ONLY:iface2>'
main LINK_LIBRARIES: 'static1;object1'
main INTERFACE_LINK_LIBRARIES: ''
]])
string(REGEX REPLACE "\r\n" "\n" expect "${expect}")
string(REGEX REPLACE "\n+$" "" expect "${expect}")

file(READ "${out}" actual)
string(REGEX REPLACE "\r\n" "\n" actual "${actual}")
string(REGEX REPLACE "\n+$" "" actual "${actual}")

if(NOT actual MATCHES "^${expect}$")
  string(REPLACE "\n" "\n expect> " expect " expect> ${expect}")
  string(REPLACE "\n" "\n actual> " actual " actual> ${actual}")
  message(FATAL_ERROR "Expected file(GENERATE) output:\n${expect}\ndoes not match actual output:\n${actual}")
endif()
