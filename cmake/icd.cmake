if(BUILD_ICD)
	message(STATUS "Building with ICD support")

	if(NOT CROSS_COMPILE)
		# accoding to pocl, pkg-config doesn't work with cross-compiling
		# Debian does not provide a "ocl-icd" pkg-config module (only the generic "OpenCL" one), so do not require it
		# but fall back to the "OpenCL" module and finally to the CMake find module
		pkg_search_module(OCL_ICD ocl-icd>=1.3)
		if(NOT OCL_ICD_FOUND)
			message(STATUS "ocl-icd>=1.3 not found via pkg-config, falling back to the OpenCL pkg-config module")
			pkg_search_module(OCL_ICD OpenCL)
		endif()
		if(NOT OCL_ICD_FOUND)
			message(STATUS "No OpenCL pkg-config module found, falling back to find_package(OpenCL)")
			if(NOT OpenCL_FOUND)
				find_package(OpenCL REQUIRED)
			endif()
			set(OCL_ICD_FOUND TRUE)
			set(OCL_ICD_INCLUDEDIR "${OpenCL_INCLUDE_DIRS}")
			set(OCL_ICD_LIBDIR "${OpenCL_LIBRARY}")
			set(OCL_ICD_VERSION "${OpenCL_VERSION_STRING}")
		endif()
		if(OCL_ICD_FOUND AND NOT OCL_ICD_INCLUDEDIR)
			# some FindPkgConfig versions only set the plural include variable
			set(OCL_ICD_INCLUDEDIR "${OCL_ICD_INCLUDE_DIRS}")
		endif()
		message(STATUS "Found Khronos ICD Loader in version ${OCL_ICD_VERSION} in ${OCL_ICD_LIBDIR} (headers: ${OCL_ICD_INCLUDEDIR})")
	endif()

	# Check clCreateProgramWithIL exist or not
	set(CMAKE_REQUIRED_LIBRARIES OpenCL)
	check_symbol_exists(clCreateProgramWithIL ${SYSROOT_CROSS}/usr/include/CL/cl.h AVAIL_clCreateProgramWithIL)
	if(NOT AVAIL_clCreateProgramWithIL)
		message(WARNING "clCreateProgramWithIL not found, strongly recommend to upgrade package opencl-headers!")
	endif()
	set(CMAKE_REQUIRED_LIBRARIES)
endif()
