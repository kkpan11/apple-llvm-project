add_lldb_library(lldbPluginScriptedProcess PLUGIN
  ScriptedProcess.cpp
  ScriptedThread.cpp
  ScriptedFrame.cpp

  LINK_LIBS
    lldbCore
    lldbTarget
    lldbUtility
    lldbPluginProcessUtility
  LINK_COMPONENTS
    BinaryFormat
    Object
    Support
  )
