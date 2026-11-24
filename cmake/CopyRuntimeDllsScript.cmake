# cmake -DDLLS=<a;b;...> -DDST=<dir> -P CopyRuntimeDllsScript.cmake
# Copies every existing file of DLLS into DST (an empty list is fine).
foreach(_dll IN LISTS DLLS)
    if(_dll AND EXISTS "${_dll}")
        file(COPY "${_dll}" DESTINATION "${DST}")
    endif()
endforeach()
