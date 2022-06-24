#pragma once

#include <skygfx/skygfx.h>

#include <GLFW/glfw3.h>

#if defined(WIN32)
	#define GLFW_EXPOSE_NATIVE_WIN32
#elif defined(__APPLE__)
	#define GLFW_EXPOSE_NATIVE_COCOA
#endif
#include <GLFW/glfw3native.h>

namespace utils
{
	skygfx::BackendType ChooseBackendTypeViaConsole()
	{
		std::cout << "Choose backend type: " << std::endl;
		std::cout << "1. D3D11" << std::endl;
		std::cout << "2. OpenGL 4.4" << std::endl;
		std::cout << "3. Vulkan" << std::endl;
		std::cout << "4. Metal" << std::endl;

		int value = 0;
		std::cin >> value;

		if (value == 1)
			return skygfx::BackendType::D3D11;
		else if (value == 2)
			return skygfx::BackendType::OpenGL44;
		else if (value == 3)
			return skygfx::BackendType::Vulkan;
		else if (value == 4)
			return skygfx::BackendType::Metal;
		else
			throw std::runtime_error("unknown type");
	}

	void* GetNativeWindow(GLFWwindow* window)
	{
#if defined(GLFW_EXPOSE_NATIVE_WIN32)
		return glfwGetWin32Window(window);
#elif defined(GLFW_EXPOSE_NATIVE_COCOA)
		return glfwGetCocoaWindow(window);
#endif
		return nullptr;
	}
}
