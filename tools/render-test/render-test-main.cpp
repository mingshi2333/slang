// render-test-main.cpp

#define _CRT_SECURE_NO_WARNINGS 1

#include "options.h"
#include "render.h"

#include "slang-support.h"
#include "surface.h"
#include "png-serialize-util.h"

#include "shader-renderer-util.h"

#include "../source/core/slang-io.h"

#include "core/slang-token-reader.h"

#include "shader-input-layout.h"
#include <stdio.h>
#include <stdlib.h>

#include "window.h"

#include "../../source/core/slang-test-tool-util.h"

#include "cpu-compute-util.h"

namespace renderer_test {

using Slang::Result;

int gWindowWidth = 1024;
int gWindowHeight = 768;

//
// For the purposes of a small example, we will define the vertex data for a
// single triangle directly in the source file. It should be easy to extend
// this example to load data from an external source, if desired.
//

struct Vertex
{
    float position[3];
    float color[3];
    float uv[2];
};

static const Vertex kVertexData[] =
{
    { { 0,  0, 0.5 }, {1, 0, 0} , {0, 0} },
    { { 0,  1, 0.5 }, {0, 0, 1} , {1, 0} },
    { { 1,  0, 0.5 }, {0, 1, 0} , {1, 1} },
};
static const int kVertexCount = SLANG_COUNT_OF(kVertexData);

using namespace Slang;

class RenderTestApp : public WindowListener
{
	public:

    // WindowListener
    virtual Result update(Window* window) SLANG_OVERRIDE;

        // At initialization time, we are going to load and compile our Slang shader
        // code, and then create the API objects we need for rendering.
    Result initialize(SlangSession* session, Renderer* renderer, const Options& options, const ShaderCompilerUtil::Input& input);
    void runCompute();
    void renderFrame();
    void finalize();

	BindingStateImpl* getBindingState() const { return m_bindingState; }

    Result writeBindingOutput(const char* fileName);

    Result writeScreen(const char* filename);

	protected:
		/// Called in initialize
	Result _initializeShaders(SlangSession* session, Renderer* renderer, Options::ShaderProgramType shaderType, const ShaderCompilerUtil::Input& input);

	// variables for state to be used for rendering...
	uintptr_t m_constantBufferSize, m_computeResultBufferSize;

	RefPtr<Renderer> m_renderer;

	RefPtr<BufferResource>	m_constantBuffer;
	RefPtr<InputLayout>     m_inputLayout;
	RefPtr<BufferResource>  m_vertexBuffer;
	RefPtr<ShaderProgram>   m_shaderProgram;
    RefPtr<PipelineState>   m_pipelineState;
	RefPtr<BindingStateImpl>    m_bindingState;

	ShaderInputLayout m_shaderInputLayout;              ///< The binding layout
    int m_numAddedConstantBuffers;                      ///< Constant buffers can be added to the binding directly. Will be added at the end.

    Options m_options;
};

SlangResult RenderTestApp::initialize(SlangSession* session, Renderer* renderer, const Options& options, const ShaderCompilerUtil::Input& input)
{
    m_options = options;

    SLANG_RETURN_ON_FAIL(_initializeShaders(session, renderer, options.shaderType, input));

    m_numAddedConstantBuffers = 0;
	m_renderer = renderer;

    // TODO(tfoley): use each API's reflection interface to query the constant-buffer size needed
    m_constantBufferSize = 16 * sizeof(float);

    BufferResource::Desc constantBufferDesc;
    constantBufferDesc.init(m_constantBufferSize);
    constantBufferDesc.cpuAccessFlags = Resource::AccessFlag::Write;

    m_constantBuffer = renderer->createBufferResource(Resource::Usage::ConstantBuffer, constantBufferDesc);
    if (!m_constantBuffer)
        return SLANG_FAIL;

    //! Hack -> if doing a graphics test, add an extra binding for our dynamic constant buffer
    //
    // TODO: Should probably be more sophisticated than this - with 'dynamic' constant buffer/s binding always being specified
    // in the test file
    RefPtr<BufferResource> addedConstantBuffer;
    switch(m_options.shaderType)
    {
    default:
        break;

    case Options::ShaderProgramType::Graphics:
    case Options::ShaderProgramType::GraphicsCompute:
        addedConstantBuffer = m_constantBuffer;
        m_numAddedConstantBuffers++;
        break;
    }

    BindingStateImpl* bindingState = nullptr;
    SLANG_RETURN_ON_FAIL(ShaderRendererUtil::createBindingState(m_shaderInputLayout, m_renderer, addedConstantBuffer, &bindingState));
    m_bindingState = bindingState;

    // Do other initialization that doesn't depend on the source language.

    // Input Assembler (IA)

    const InputElementDesc inputElements[] = {
        { "A", 0, Format::RGB_Float32, offsetof(Vertex, position) },
        { "A", 1, Format::RGB_Float32, offsetof(Vertex, color) },
        { "A", 2, Format::RG_Float32, offsetof(Vertex, uv) },
    };

    m_inputLayout = renderer->createInputLayout(inputElements, SLANG_COUNT_OF(inputElements));
    if(!m_inputLayout)
        return SLANG_FAIL;

    BufferResource::Desc vertexBufferDesc;
    vertexBufferDesc.init(kVertexCount * sizeof(Vertex));

    m_vertexBuffer = renderer->createBufferResource(Resource::Usage::VertexBuffer, vertexBufferDesc, kVertexData);
    if(!m_vertexBuffer)
        return SLANG_FAIL;

    {
        switch(m_options.shaderType)
        {
        default:
            assert(!"unexpected test shader type");
            return SLANG_FAIL;

        case Options::ShaderProgramType::Compute:
            {
                ComputePipelineStateDesc desc;
                desc.pipelineLayout = m_bindingState->pipelineLayout;
                desc.program = m_shaderProgram;

                m_pipelineState = renderer->createComputePipelineState(desc);
            }
            break;

        case Options::ShaderProgramType::Graphics:
        case Options::ShaderProgramType::GraphicsCompute:
            {
                GraphicsPipelineStateDesc desc;
                desc.pipelineLayout = m_bindingState->pipelineLayout;
                desc.program = m_shaderProgram;
                desc.inputLayout = m_inputLayout;
                desc.renderTargetCount = m_bindingState->m_numRenderTargets;

                m_pipelineState = renderer->createGraphicsPipelineState(desc);
            }
            break;
        }
    }

    // If success must have a pipeline state
    return m_pipelineState ? SLANG_OK : SLANG_FAIL;
}

Result RenderTestApp::_initializeShaders(SlangSession* session, Renderer* renderer, Options::ShaderProgramType shaderType, const ShaderCompilerUtil::Input& input)
{
    ShaderCompilerUtil::OutputAndLayout output;
    SLANG_RETURN_ON_FAIL(ShaderCompilerUtil::compileWithLayout(session, gOptions.sourcePath, shaderType, input,  output));
    m_shaderInputLayout = output.layout;
    m_shaderProgram = renderer->createProgram(output.output.desc);
    return m_shaderProgram ? SLANG_OK : SLANG_FAIL;
}

void RenderTestApp::renderFrame()
{
    auto mappedData = m_renderer->map(m_constantBuffer, MapFlavor::WriteDiscard);
    if(mappedData)
    {
        const ProjectionStyle projectionStyle = RendererUtil::getProjectionStyle(m_renderer->getRendererType());
        RendererUtil::getIdentityProjection(projectionStyle, (float*)mappedData);

		m_renderer->unmap(m_constantBuffer);
    }

    auto pipelineType = PipelineType::Graphics;

    m_renderer->setPipelineState(pipelineType, m_pipelineState);

	m_renderer->setPrimitiveTopology(PrimitiveTopology::TriangleList);
	m_renderer->setVertexBuffer(0, m_vertexBuffer, sizeof(Vertex));

    m_bindingState->apply(m_renderer, pipelineType);

	m_renderer->draw(3);
}

void RenderTestApp::runCompute()
{
    auto pipelineType = PipelineType::Compute;
    m_renderer->setPipelineState(pipelineType, m_pipelineState);
    m_bindingState->apply(m_renderer, pipelineType);
	m_renderer->dispatchCompute(1, 1, 1);
}

void RenderTestApp::finalize()
{
}

Result RenderTestApp::writeBindingOutput(const char* fileName)
{
    // Submit the work
    m_renderer->submitGpuWork();
    // Wait until everything is complete
    m_renderer->waitForGpu();

    FILE * f = fopen(fileName, "wb");
    if (!f)
    {
        return SLANG_FAIL;
    }

    for(auto binding : m_bindingState->outputBindings)
    {
        auto i = binding.entryIndex;
        const auto& layoutBinding = m_shaderInputLayout.entries[i];

        assert(layoutBinding.isOutput);
        {
            if (binding.resource && binding.resource->isBuffer())
            {
                BufferResource* bufferResource = static_cast<BufferResource*>(binding.resource.Ptr());
                const size_t bufferSize = bufferResource->getDesc().sizeInBytes;

                unsigned int* ptr = (unsigned int*)m_renderer->map(bufferResource, MapFlavor::HostRead);
                if (!ptr)
                {
                    fclose(f);
                    return SLANG_FAIL;
                }

                const int size = int(bufferSize / sizeof(unsigned int));
                for (int i = 0; i < size; ++i)
                {
                    fprintf(f, "%X\n", ptr[i]);
                }
                m_renderer->unmap(bufferResource);
            }
            else
            {
                printf("invalid output type at %d.\n", int(i));
            }
        }
    }
    fclose(f);

    return SLANG_OK;
}


Result RenderTestApp::writeScreen(const char* filename)
{
    Surface surface;
    SLANG_RETURN_ON_FAIL(m_renderer->captureScreenSurface(surface));
    return PngSerializeUtil::write(filename, surface);
}

Result RenderTestApp::update(Window* window)
{
    // Whenever we don't have Windows events to process, we render a frame.
    if (m_options.shaderType == Options::ShaderProgramType::Compute)
    {
        runCompute();
    }
    else
    {
        static const float kClearColor[] = { 0.25, 0.25, 0.25, 1.0 };
        m_renderer->setClearColor(kClearColor);
        m_renderer->clearFrame();

        renderFrame();
    }

    // If we are in a mode where output is requested, we need to snapshot the back buffer here
    if (m_options.outputPath)
    {
        // Submit the work
        m_renderer->submitGpuWork();
        // Wait until everything is complete
        m_renderer->waitForGpu();

        if (gOptions.shaderType == Options::ShaderProgramType::Compute || gOptions.shaderType == Options::ShaderProgramType::GraphicsCompute)
        {
            SLANG_RETURN_ON_FAIL(writeBindingOutput(gOptions.outputPath));
        }
        else
        {
            SlangResult res = writeScreen(gOptions.outputPath);
            if (SLANG_FAILED(res))
            {
                fprintf(stderr, "ERROR: failed to write screen capture to file\n");
                return res;
            }
        }
        // We are done
        window->postQuit();
        return SLANG_OK;
    }

    m_renderer->presentFrame();
    return SLANG_OK;
}

} //  namespace renderer_test

SLANG_TEST_TOOL_API SlangResult innerMain(Slang::StdWriters* stdWriters, SlangSession* session, int argcIn, const char*const* argvIn)
{
    using namespace renderer_test;
    using namespace Slang;

    StdWriters::setSingleton(stdWriters);

	// Parse command-line options
	SLANG_RETURN_ON_FAIL(parseOptions(argcIn, argvIn, StdWriters::getError()));

    // Declare window pointer before renderer, such that window is released after renderer
    RefPtr<renderer_test::Window> window;

    ShaderCompilerUtil::Input input;
    
    input.profile = "";
    input.target = SLANG_TARGET_NONE;
    input.args = &gOptions.slangArgs[0];
    input.argCount = gOptions.slangArgCount;

	SlangSourceLanguage nativeLanguage = SLANG_SOURCE_LANGUAGE_UNKNOWN;
	SlangPassThrough slangPassThrough = SLANG_PASS_THROUGH_NONE;
    char const* profileName = "";
	switch (gOptions.rendererType)
	{
		case RendererType::DirectX11:
			input.target = SLANG_DXBC;
            input.profile = "sm_5_0";
			nativeLanguage = SLANG_SOURCE_LANGUAGE_HLSL;
            slangPassThrough = SLANG_PASS_THROUGH_FXC;
            
			break;

		case RendererType::DirectX12:
			input.target = SLANG_DXBC;
            input.profile = "sm_5_0";
			nativeLanguage = SLANG_SOURCE_LANGUAGE_HLSL;
            slangPassThrough = SLANG_PASS_THROUGH_FXC;
            
            if( gOptions.useDXIL )
            {
                input.target = SLANG_DXIL;
                input.profile = "sm_6_0";
                slangPassThrough = SLANG_PASS_THROUGH_DXC;
            }
			break;

		case RendererType::OpenGl:
			input.target = SLANG_GLSL;
            input.profile = "glsl_430";
			nativeLanguage = SLANG_SOURCE_LANGUAGE_GLSL;
            slangPassThrough = SLANG_PASS_THROUGH_GLSLANG;
			break;

		case RendererType::Vulkan:
			input.target = SLANG_SPIRV;
            input.profile = "glsl_430";
			nativeLanguage = SLANG_SOURCE_LANGUAGE_GLSL;
            slangPassThrough = SLANG_PASS_THROUGH_GLSLANG;
			break;
        case RendererType::CPU:
            input.target = SLANG_HOST_CALLABLE;
            input.profile = "";
            nativeLanguage = SLANG_SOURCE_LANGUAGE_CPP;
            slangPassThrough = SLANG_PASS_THROUGH_GENERIC_C_CPP;
            break;
		default:
			fprintf(stderr, "error: unexpected\n");
			return SLANG_FAIL;
	}

    switch (gOptions.inputLanguageID)
    {
        case Options::InputLanguageID::Slang:
            input.sourceLanguage = SLANG_SOURCE_LANGUAGE_SLANG;
            input.passThrough = SLANG_PASS_THROUGH_NONE;
            break;

        case Options::InputLanguageID::Native:
            input.sourceLanguage = nativeLanguage;
            input.passThrough = slangPassThrough;
            break;

        default:
            break;
    }

    // Use the profile name set on options if set
    input.profile = gOptions.profileName ? gOptions.profileName : input.profile;

    StringBuilder rendererName;
    rendererName << "[" << RendererUtil::toText(gOptions.rendererType) << "] ";
    if (gOptions.adapter.getLength())
    {
        rendererName << "'" << gOptions.adapter << "'";
    }

    // If it's CPU testing we don't need a window or a renderer
    if (gOptions.rendererType == RendererType::CPU)
    {
        if (gOptions.onlyStartup)
        {
            // Need generic C/C++
            if (SLANG_FAILED(spSessionCheckPassThroughSupport(session, SLANG_PASS_THROUGH_GENERIC_C_CPP)))
            {
                return SLANG_FAIL;
            }
            // Should work ... 
            return SLANG_OK;
        }

        ShaderCompilerUtil::OutputAndLayout compilationAndLayout;
        SLANG_RETURN_ON_FAIL(ShaderCompilerUtil::compileWithLayout(session, gOptions.sourcePath, gOptions.shaderType, input, compilationAndLayout));

        CPUComputeUtil::Context context;
        SLANG_RETURN_ON_FAIL(CPUComputeUtil::calcBindings(compilationAndLayout, context));
        SLANG_RETURN_ON_FAIL(CPUComputeUtil::execute(compilationAndLayout, context));

        // Dump everything out that was written
        return CPUComputeUtil::writeBindings(compilationAndLayout.layout, context.buffers, gOptions.outputPath);
    }

    // Renderer is constructed (later) using the window
    Slang::RefPtr<Renderer> renderer;

    {
        RendererUtil::CreateFunc createFunc = RendererUtil::getCreateFunc(gOptions.rendererType);
        if (createFunc)
        {
            renderer = createFunc();
        }

        if (!renderer)
        {
            if (!gOptions.onlyStartup)
            {
                fprintf(stderr, "Unable to create renderer %s\n", rendererName.getBuffer());
            }
            return SLANG_FAIL;
        }

        Renderer::Desc desc;
        desc.width = gWindowWidth;
        desc.height = gWindowHeight;
        desc.adapter = gOptions.adapter;

        window = renderer_test::Window::create();
        SLANG_RETURN_ON_FAIL(window->initialize(gWindowWidth, gWindowHeight));

        SlangResult res = renderer->initialize(desc, window->getHandle());
        if (SLANG_FAILED(res))
        {
            if (!gOptions.onlyStartup)
            {
                fprintf(stderr, "Unable to initialize renderer %s\n", rendererName.getBuffer());
            }
            return res;
        }

        for (const auto& feature : gOptions.renderFeatures)
        {
            // If doesn't have required feature... we have to give up
            if (!renderer->hasFeature(feature.getUnownedSlice()))
            {
                return SLANG_E_NOT_AVAILABLE;
            }
        }
    }
   
    // If the only test is we can startup, then we are done
    if (gOptions.onlyStartup)
    {
        return SLANG_OK;
    }

	{
		RefPtr<RenderTestApp> app(new RenderTestApp);
		SLANG_RETURN_ON_FAIL(app->initialize(session, renderer, gOptions, input));
        window->show();
        return window->runLoop(app);
	}
}

int main(int argc, char**  argv)
{
    using namespace Slang;
    SlangSession* session = spCreateSession(nullptr);

    TestToolUtil::setSessionDefaultPrelude(argv[0], session);
    
    auto stdWriters = StdWriters::initDefaultSingleton();
    
    SlangResult res = innerMain(stdWriters, session, argc, argv);
    spDestroySession(session);

	return (int)TestToolUtil::getReturnCode(res);
}

