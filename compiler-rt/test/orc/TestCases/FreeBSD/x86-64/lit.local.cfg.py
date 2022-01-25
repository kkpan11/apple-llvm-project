<<<<<<< HEAD
if config.root.host_arch not in ['x86_64', 'amd64']:
  config.unsupported = True
||||||| 11b7ee974a69
=======
if config.root.host_arch not in ['x86_64', 'amd64']:
  config.unsupported = True

if config.target_arch not in ['x86_64', 'amd64']:
  config.unsupported = True
>>>>>>> llvm.org/main
