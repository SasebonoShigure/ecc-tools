# Full oracles, fault injection and audit contracts require explicit diagnostics.
# Disabled tests remain visible as Not Run in the OFF build; they are never passes.
function(ista_diagnostic_tests)
  foreach(test IN LISTS ARGN)
    set_property(TEST "${test}" APPEND PROPERTY LABELS ista_diagnostic)
    if(NOT ECC_INCREGPUSTA_DEBUG_CHECKS)
      set_tests_properties("${test}" PROPERTIES DISABLED TRUE)
    endif()
  endforeach()
endfunction()
