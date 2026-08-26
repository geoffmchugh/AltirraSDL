if(NOT ALTIRRA_BUILD_TESTS)
	return()
endif()

add_executable(altirra-bridge-pokey-audio-test
	bridge_pokey_audio_test.cpp)

target_include_directories(altirra-bridge-pokey-audio-test PRIVATE
	${CMAKE_CURRENT_SOURCE_DIR}/..)
target_compile_features(altirra-bridge-pokey-audio-test PRIVATE cxx_std_17)

add_test(NAME bridge-pokey-audio
	COMMAND altirra-bridge-pokey-audio-test)
