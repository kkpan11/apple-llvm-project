import lldbsuite.test.lldbinline as lldbinline
from lldbsuite.test.decorators import *

# SWIFT_ENABLE_TENSORFLOW
# TODO(TF-1347): Find a better workaround in Swift for `SIMDVector`.
# lldbinline.MakeInlineTest(__file__, globals(), decorators=[swiftTest])
# SWIFT_ENABLE_TENSORFLOW END
