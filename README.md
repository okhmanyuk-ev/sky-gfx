# sky-gfx - Lightweight rendering library

# Features
- Backend API: D3D11, OpenGL 4.4
- GLSL shaders for any backend via SPIRV-Cross
- RAII memory management over objects like Device, Shader, Texture, etc..
- Choosing backend API in runtime, no compilation definitions

# Integration

```cmake
add_subdirectory(sky-gfx)
target_link_libraries(YOUR_TARGET sky-gfx)
```

# Building Examples

## Visual Studio
1. Open the `examples` folder
2. Execute `build.cmd`
3. The `build` folder will be created in a short time and the `.sln` file will be inside

# Examples

## Triangle

![image](https://user-images.githubusercontent.com/3295141/173175376-c33d287d-4cc5-4070-9f08-d1379b6b4374.png)

## Texture

![image](https://user-images.githubusercontent.com/3295141/173175982-79d1f92f-76bf-4dea-adf2-973f66db4b02.png)

## Uniform Buffer

![a (4)](https://user-images.githubusercontent.com/3295141/173176075-7fdb9759-e3ca-4447-b439-2acd27f7ced9.gif)

## Cube

![a (5)](https://user-images.githubusercontent.com/3295141/173178283-083e54c7-488d-457f-91f1-e4685ecc3538.gif)

## Textured Cube

![a (7)](https://user-images.githubusercontent.com/3295141/173178767-7bafba29-96f2-4d44-8067-04fc1fb872a2.gif)

## Render Target

![a (8)](https://user-images.githubusercontent.com/3295141/173179661-e9e9947d-bc46-41a6-82f9-cf9041ce56c2.gif)
