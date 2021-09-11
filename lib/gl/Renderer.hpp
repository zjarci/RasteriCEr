// RasteriCEr
// https://github.com/ToNi3141/RasteriCEr
// Copyright (c) 2021 ToNi3141

// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.

// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#ifndef RENDERER_HPP
#define RENDERER_HPP

#include <stdint.h>
#include <array>
#include "Vec.hpp"
#include "IRenderer.hpp"
#include "IBusConnector.hpp"
#include "DisplayList.hpp"
#include "Rasterizer.hpp"
#include <string.h>

// Screen
// <-----------------X_RESOLUTION--------------------------->
// +--------------------------------------------------------+ ^
// |        ^                                               | |
// |        | LINE_RESOLUTION        DISPLAY_LINES          | |
// |        |                                               | |
// |        v                                               | |
// |<------------------------------------------------------>| Y
// |                                                        | _
// |                                 DISPLAY_LINES          | R
// |                                                        | E
// |                                                        | S
// |<------------------------------------------------------>| O
// |                                                        | L
// |                                 DISPLAY_LINES          | U
// |                                                        | T
// |                                                        | I
// |<------------------------------------------------------>| O
// |                                                        | N
// |                                 DISPLAY_LINES          | |
// |                                                        | |
// |                                                        | |
// +--------------------------------------------------------+ v
// This renderer collects all triangles in a single display list. Later, when the display list is uploaded, this renderer
// will create sub display lists for each display line. This approach is more memory efficient because every triangle is saved
// only once, but because it has to reinterpret the display list during upload, it is slower then the approach in the
// RendererBuckets. This renderer will preallocate for every display line one display list and will dispatch each incoming
// triangle to the buckets. It should be faster but it is less memory efficient because a triangle is potentially saved
// several times.
// The BUS_WIDTH is used to calculate the alignment in the display list.
template <uint32_t DISPLAY_LIST_SIZE = 2048, uint16_t DISPLAY_LINES = 1, uint16_t LINE_RESOLUTION = 128, uint16_t BUS_WIDTH = 32>
class Renderer : public IRenderer
{
public:
    Renderer(IBusConnector& busConnector)
        : m_busConnector(busConnector)
    {
        m_displayList[0].clear();
        m_displayList[1].clear();

        // Unfortunately the Arduino compiler is too old and does not support C++20 default member initializers in bit fields
#ifndef NO_PERSP_CORRECT
        m_confReg2.perspectiveCorrectedTextures = true;
#else
        m_confReg2.perspectiveCorrectedTextures = false;
#endif

        setDepthFunc(TestFunc::LESS);
        setDepthMask(false);
        setColorMask(true, true, true, true);
        setAlphaFunc(TestFunc::ALWAYS, 0xf);
        setTexEnv(TexEnvTarget::TEXTURE_ENV, TexEnvParamName::TEXTURE_ENV_MODE, TexEnvParam::MODULATE);
        setBlendFunc(BlendFunc::ONE, BlendFunc::ZERO);
        setLogicOp(LogicOp::COPY);
        setTexEnvColor({{0, 0, 0, 0}});
        setClearColor({{0, 0, 0, 0}});
        setClearDepth(65535);
    }

    virtual bool drawTriangle(const Vec4& v0,
                              const Vec4& v1,
                              const Vec4& v2,
                              const Vec2& st0,
                              const Vec2& st1,
                              const Vec2& st2,
                              const Vec4i& color) override
    {
        Rasterizer::RasterizedTriangle triangleConf;

        if (!Rasterizer::rasterize(triangleConf, v0, st0, v1, st1, v2, st2))
        {
            // Triangle is not visible
            return true;
        }

        triangleConf.triangleStaticColor = convertColor(color);

        bool retVal = appendStreamCommand(m_displayList[m_backList], StreamCommand::TRIANGLE_FULL, triangleConf);
        // Should have a really low performance impact to trigger a upload after each triangle...
        uploadDisplayList();
        return retVal;
    }

    virtual void commit() override
    {
        // Add frame buffer flush command
        SCT *op = m_displayList[m_backList].template create<SCT>();
        if (op)
        {
            *op = StreamCommand::FRAMEBUFFER_COMMIT | StreamCommand::FRAMEBUFFER_COLOR;
        }
        else
        {
            // In case it was not possible to add the clear and commit command, discard all lists to stay in
            // sync with the display output. Otherwise the hardware will not send the current framebuffer slice
            // to the frame buffer which causes that the image will move because of skiped lines
            m_displayList[m_backList].clear();
            return;
        }

        // Check if all front display lists are empty
        // If no display list is empty, block as long as all the lists from the frontList are transferred
        while (uploadDisplayList())
            ;

        // Enqueue all lists from the back display list
        m_displayList[m_backList].enqueue();

        // Switch the display lists
        if (m_backList == 0)
        {
            m_backList = 1;
            m_frontList = 0;
        }
        else
        {
            m_backList = 0;
            m_frontList = 1;
        }

        // Triggers an upload
        uploadDisplayList();
    }

    virtual bool useTexture(const uint16_t* pixels, const uint16_t texWidth, const uint16_t texHeight) override
    {
        // Right now only support for square textures
        if (texWidth != texHeight)
            return false;

        SCT op;
        TextureStreamArg tsa;

        if (texWidth == 256)
            op = StreamCommand::TEXTURE_STREAM_256x256;
        else if (texWidth == 128)
            op = StreamCommand::TEXTURE_STREAM_128x128;
        else if (texWidth == 64)
            op = StreamCommand::TEXTURE_STREAM_64x64;
        else if (texWidth == 32)
            op = StreamCommand::TEXTURE_STREAM_32x32;
        else
            return false; // Not supported texture format

        tsa.remainingPixels = texWidth * texHeight;
        tsa.pixels = pixels;
        return appendStreamCommand(op, tsa);
    }

    virtual bool clear(bool colorBuffer, bool depthBuffer) override
    {
        const SCT opColorBuffer = StreamCommand::FRAMEBUFFER_MEMSET | StreamCommand::FRAMEBUFFER_COLOR;
        const SCT opDepthBuffer = StreamCommand::FRAMEBUFFER_MEMSET | StreamCommand::FRAMEBUFFER_DEPTH;

        SCT *op = m_displayList[m_backList].template create<SCT>();
        if (op)
        {
            if (colorBuffer && depthBuffer)
            {
                *op = opColorBuffer | opDepthBuffer;
            }
            else if (colorBuffer)
            {
                *op = opColorBuffer;
            }
            else if (depthBuffer)
            {
                *op = opDepthBuffer;
            }
            else
            {
                *op = StreamCommand::NOP;
            }
        }
        return op != nullptr;
    }

    virtual bool setClearColor(const Vec4i& color) override
    {
        return appendStreamCommand(m_displayList[m_backList],
                                   StreamCommand::SET_COLOR_BUFFER_CLEAR_COLOR, convertColor(color));
    }

    virtual bool setClearDepth(uint16_t depth) override
    {
        return appendStreamCommand(m_displayList[m_backList], StreamCommand::SET_DEPTH_BUFFER_CLEAR_DEPTH, depth);
    }

    virtual bool setDepthMask(const bool flag) override
    {
        m_confReg1.depthMask = flag;
        return appendStreamCommand(StreamCommand::SET_CONF_REG1, m_confReg1);
    }

    virtual bool enableDepthTest(const bool enable) override
    {
        m_confReg1.enableDepthTest = enable;
        return appendStreamCommand(StreamCommand::SET_CONF_REG1, m_confReg1);
    }

    virtual bool setColorMask(const bool r, const bool g, const bool b, const bool a) override
    {
        m_confReg1.colorMaskA = a;
        m_confReg1.colorMaskB = b;
        m_confReg1.colorMaskG = g;
        m_confReg1.colorMaskR = r;
        return appendStreamCommand(StreamCommand::SET_CONF_REG1, m_confReg1);
    }

    virtual bool setDepthFunc(const TestFunc func) override
    {
        m_confReg1.depthFunc = func;
        return appendStreamCommand(StreamCommand::SET_CONF_REG1, m_confReg1);
    }

    virtual bool setAlphaFunc(const TestFunc func, const uint8_t ref) override
    {
        m_confReg1.alphaFunc = func;
        m_confReg1.referenceAlphaValue = ref;
        return appendStreamCommand(StreamCommand::SET_CONF_REG1, m_confReg1);
    }

    virtual bool setTexEnv(const TexEnvTarget target, const TexEnvParamName pname, const TexEnvParam param) override
    {
        (void)target; // Only TEXTURE_ENV is supported
        (void)pname; // Only GL_TEXTURE_ENV_MODE is supported
        m_confReg2.texEnvFunc = param;
        return appendStreamCommand(StreamCommand::SET_CONF_REG2, m_confReg2);
    }

    virtual bool setBlendFunc(const BlendFunc sfactor, const BlendFunc dfactor) override
    {
        m_confReg2.blendFuncSFactor = sfactor;
        m_confReg2.blendFuncDFactor = dfactor;
        return appendStreamCommand(StreamCommand::SET_CONF_REG2, m_confReg2);
    }

    virtual bool setLogicOp(const LogicOp opcode) override
    {
        (void)opcode;
        return false;
    }

    virtual bool setTexEnvColor(const Vec4i& color) override
    {
        return appendStreamCommand(StreamCommand::SET_TEX_ENV_COLOR, convertColor(color));
    }

    virtual bool setTextureWrapModeS(const TextureWrapMode mode)  override
    {
        m_confReg2.texClampS = mode == TextureWrapMode::CLAMP_TO_EDGE;
        return appendStreamCommand(StreamCommand::SET_CONF_REG2, m_confReg2);
    }

    virtual bool setTextureWrapModeT(const TextureWrapMode mode) override
    {
        m_confReg2.texClampT = mode == TextureWrapMode::CLAMP_TO_EDGE;
        return appendStreamCommand(StreamCommand::SET_CONF_REG2, m_confReg2);
    }

private:
    static constexpr uint32_t HARDWARE_BUFFER_SIZE = 2048;
    static constexpr uint32_t DISPLAY_BUFFERS = 2; // Note: Right now only two are supported. Other values will not work

    using List = DisplayList<DISPLAY_LIST_SIZE, BUS_WIDTH / 8>;
    using ListUpload = DisplayList<HARDWARE_BUFFER_SIZE, BUS_WIDTH / 8>;

    struct StreamCommand
    {
        // Anathomy of a command:
        // | 4 bit OP | 12 bit IMM |

        using StreamCommandType = uint16_t;

        // This mask will set the command
        static constexpr StreamCommandType STREAM_COMMAND_OP_MASK = 0xf000;

        // This mask will set the immediate value
        static constexpr StreamCommandType STREAM_COMMAND_IMM_MASK = 0x0fff;

        // Calculate the triangle size with align overhead.
        static constexpr StreamCommandType TRIANGLE_SIZE_ALIGNED = ListUpload::template sizeOf<Rasterizer::RasterizedTriangle>();

        // OPs
        static constexpr StreamCommandType NOP              = 0x0000;
        static constexpr StreamCommandType TEXTURE_STREAM   = 0x1000;
        static constexpr StreamCommandType SET_REG          = 0x2000;
        static constexpr StreamCommandType FRAMEBUFFER_OP   = 0x3000;
        static constexpr StreamCommandType TRIANGLE_STREAM  = 0x4000;

        // Immediate values
        static constexpr StreamCommandType TEXTURE_STREAM_32x32     = TEXTURE_STREAM | 0x0011;
        static constexpr StreamCommandType TEXTURE_STREAM_64x64     = TEXTURE_STREAM | 0x0022;
        static constexpr StreamCommandType TEXTURE_STREAM_128x128   = TEXTURE_STREAM | 0x0044;
        static constexpr StreamCommandType TEXTURE_STREAM_256x256   = TEXTURE_STREAM | 0x0088;

        static constexpr StreamCommandType SET_COLOR_BUFFER_CLEAR_COLOR = SET_REG | 0x0000;
        static constexpr StreamCommandType SET_DEPTH_BUFFER_CLEAR_DEPTH = SET_REG | 0x0001;
        static constexpr StreamCommandType SET_CONF_REG1                = SET_REG | 0x0002;
        static constexpr StreamCommandType SET_CONF_REG2                = SET_REG | 0x0003;
        static constexpr StreamCommandType SET_TEX_ENV_COLOR            = SET_REG | 0x0004;

        static constexpr StreamCommandType FRAMEBUFFER_COMMIT   = FRAMEBUFFER_OP | 0x0001;
        static constexpr StreamCommandType FRAMEBUFFER_MEMSET   = FRAMEBUFFER_OP | 0x0002;
        static constexpr StreamCommandType FRAMEBUFFER_COLOR    = FRAMEBUFFER_OP | 0x0010;
        static constexpr StreamCommandType FRAMEBUFFER_DEPTH    = FRAMEBUFFER_OP | 0x0020;

        static constexpr StreamCommandType TRIANGLE_FULL  = TRIANGLE_STREAM | TRIANGLE_SIZE_ALIGNED;
    };
    using SCT = typename StreamCommand::StreamCommandType;

    struct TextureStreamArg
    {
        const uint16_t* pixels;
        int32_t remainingPixels;
    };

    static uint16_t convertColor(const Vec4i color)
    {
        Vec4i colorShift{color};
        colorShift >>= 4;
        uint16_t colorInt =   (static_cast<uint16_t>(colorShift[3]) << 0)
                | (static_cast<uint16_t>(colorShift[2]) << 4)
                | (static_cast<uint16_t>(colorShift[1]) << 8)
                | (static_cast<uint16_t>(colorShift[0]) << 12);
        return colorInt;
    }

    /// @brief This method will try to send a new display list to the hardware, if the bus is clear.
    /// @return true if a upload is in progress
    ///         false no upload is in progress
    bool uploadDisplayList()
    {
        // Check if the bus is clear
        if (!m_busConnector.clearToSend())
            return true;

        List& frontList = m_displayList[m_frontList];
        // Check if the front list is queued. If so, initialize a new transfer
        if (frontList.state() == List::State::QUEUED)
        {
            // Upload the display lists in reverse order because in reality the rendered picture is upside down
            m_uploadIndexPosition = DISPLAY_LINES - 1;
            frontList.transfer();
        }

        if (frontList.state() == List::State::TRANSFERRING)
        {
            // First check if an texture upload is pending. If so, finish the upload first
            if (m_textureStreamArg.remainingPixels > 0)
            {
                m_busConnector.writeData(reinterpret_cast<const uint8_t*>(m_textureStreamArg.pixels), HARDWARE_BUFFER_SIZE);
                static constexpr uint32_t PIXEL_INC = (HARDWARE_BUFFER_SIZE / sizeof(m_textureStreamArg.pixels[0]));
                m_textureStreamArg.pixels += PIXEL_INC;
                m_textureStreamArg.remainingPixels -= PIXEL_INC;
                return true;
            }

            // Build new displaylist (which will be uploaded to the device)
            m_displayListUpload.clear();
            bool leaveLoop = false;
            while (!leaveLoop && hasEnoughSpace(m_displayListUpload))
            {
                SCT *op = frontList.template getNext<SCT>();
                if (op == nullptr)
                {
                    break;
                }

                *(m_displayListUpload.template create<SCT>()) = *op;
                switch ((*op) & StreamCommand::STREAM_COMMAND_OP_MASK) {
                case StreamCommand::TRIANGLE_STREAM:
                {
                    // Assume, when the op is TRIANGLE_STREAM, then this command must follow a triangle
                    Rasterizer::RasterizedTriangle *triangleConf = frontList.template getNext<Rasterizer::RasterizedTriangle>();
                    Rasterizer::RasterizedTriangle *triangleConfDl = m_displayListUpload.template create<Rasterizer::RasterizedTriangle>();
                    const uint16_t currentScreenPositionStart = m_uploadIndexPosition * LINE_RESOLUTION;
                    const uint16_t currentScreenPositionEnd = (m_uploadIndexPosition + 1) * LINE_RESOLUTION;
                    if (!Rasterizer::calcLineIncrement(*triangleConfDl, *triangleConf, currentScreenPositionStart,
                                                       currentScreenPositionEnd))
                    {
                        // Special case in case, the triangle is not visible, just remove it from the display list
                        // This case can happen when the triangle is not in the current display line
                        m_displayListUpload.template remove<Rasterizer::RasterizedTriangle>();
                        m_displayListUpload.template remove<SCT>();
                    }
                }
                    break;
                case StreamCommand::FRAMEBUFFER_OP:
                case StreamCommand::NOP:
                    // Has no argument
                    break;
                case StreamCommand::TEXTURE_STREAM:
                {
                    // Save texture upload argument and check if we have to reupload the texture
                    TextureStreamArg curr = m_textureStreamArg;
                    m_textureStreamArg = *(frontList.template getNext<TextureStreamArg>());
                    if ((m_textureStreamArg.pixels + m_textureStreamArg.remainingPixels) == curr.pixels)
                    {
                        // The texture is already on the device buffer, so fast forward
                        m_textureStreamArg.pixels += m_textureStreamArg.remainingPixels;
                        m_textureStreamArg.remainingPixels = 0;
                        // and discard command
                        m_displayListUpload.template remove<SCT>();
                    }
                    else
                    {
                        // Upload texture
                        leaveLoop = true;

                        // TODO: We could check here if the next command is also a texture upload with a different texture. If so, then we can discard
                        // that command and just upload the comming texture.
                        // We can also iterate now thru the triangles to check, if a triangle is visible in this line till the next upload command is comming
                        // if no triangle is visible, then we can also discard this command and just upload the next.
                    }
                }
                    break;
                case StreamCommand::SET_REG:
                {
                    uint16_t *arg = m_displayListUpload.template create<uint16_t>();
                    *arg = *(frontList.template getNext<uint16_t>());
                }
                    break;
                default:
                    // In case the op was not found
                    m_displayListUpload.template remove<SCT>();
                    break;
                }
            }

            m_busConnector.startColorBufferTransfer(m_uploadIndexPosition);
            m_busConnector.writeData(m_displayListUpload.getMemPtr(), m_displayListUpload.getSize());

            if (frontList.atEnd())
            {
                frontList.resetGet();
                if (m_uploadIndexPosition == 0)
                {
                    frontList.clear();
                    return false;
                }
                m_uploadIndexPosition--;
            }
            return true;
        }

        return false;
    }

    template <typename TDisplayList, typename TArg>
    bool appendStreamCommand(TDisplayList& displayList, const SCT op, const TArg& arg)
    {
        SCT *opDl = displayList.template create<SCT>();
        TArg *argDl = displayList.template create<TArg>();

        if (!(opDl && argDl))
        {
            if (opDl)
            {
                displayList.template remove<SCT>();
            }

            if (argDl)
            {
                displayList.template remove<TArg>();
            }
            // Out of memory error
            return false;
        }

        *opDl = op;
        *argDl = arg;
        return true;
    }

    template <typename TArg>
    bool appendStreamCommand(const SCT op, const TArg& arg)
    {
        return appendStreamCommand(m_displayList[m_backList], op, arg);
    }

    template <typename TDisplayList>
    bool hasEnoughSpace(const TDisplayList& displayList)
    {
        return displayList.getFreeSpace() >= (displayList.template sizeOf<SCT>() + displayList.template sizeOf<Rasterizer::RasterizedTriangle>());
    }

    std::array<List, DISPLAY_BUFFERS> m_displayList __attribute__ ((aligned (8)));
    ListUpload m_displayListUpload __attribute__ ((aligned (8)));
    uint8_t m_frontList = 0;
    uint8_t m_backList = 1;
    uint32_t m_uploadIndexPosition = 0;
    TextureStreamArg m_textureStreamArg{nullptr, 0};

    IBusConnector& m_busConnector;

    uint16_t m_staticTriangleColor = 0xffff;

    struct __attribute__ ((__packed__)) ConfReg1
    {
        bool enableDepthTest : 1;
        IRenderer::TestFunc depthFunc : 3;
        IRenderer::TestFunc alphaFunc : 3;
        uint8_t referenceAlphaValue : 4;
        bool depthMask : 1;
        bool colorMaskA : 1;
        bool colorMaskB : 1;
        bool colorMaskG : 1;
        bool colorMaskR : 1;
    } m_confReg1;

    struct __attribute__ ((__packed__)) ConfReg2
    {
        bool perspectiveCorrectedTextures : 1;
        IRenderer::TexEnvParam texEnvFunc : 3;
        IRenderer::BlendFunc blendFuncSFactor : 4;
        IRenderer::BlendFunc blendFuncDFactor : 4;
        bool texClampS : 1;
        bool texClampT : 1;
    } m_confReg2;
};

#endif // RENDERER_HPP