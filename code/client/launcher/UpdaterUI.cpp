/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"

#ifdef LAUNCHER_PERSONALITY_MAIN
#include <CommCtrl.h>
#include <shobjidl.h>

#include <ShellScalingApi.h>

#include <winrt/Windows.Storage.Streams.h>

#include "CitiLaunch/BackdropBrush.g.h"

#include <DirectXMath.h>
#include <roapi.h>

#include <CfxState.h>
#include <HostSharedData.h>

#include <boost/algorithm/string.hpp>

#pragma comment(lib, "runtimeobject.lib")
#pragma comment(lib, "delayimp.lib")
#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "d2d1.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "dwrite.lib")
#pragma comment(lib, "shcore.lib")

extern "C" DLL_EXPORT HRESULT WINRT_CALL DllGetActivationFactory(HSTRING classId, IActivationFactory** factory);

namespace winrt
{
	namespace Microsoft::Graphics::Canvas::Effects
	{
		class ColorSourceEffect;
		class Transform2DEffect;
		class ColorMatrixEffect;
		class CompositeEffect;
	}

	namespace impl
	{
		template <typename Class, typename Interface = Windows::Foundation::IActivationFactory>
		auto get_local_activation_factory(hresult_error * exception = nullptr) noexcept
		{
			param::hstring const name{ name_of<Class>() };
			impl::com_ref<Interface> object;
			hresult const hr = ::DllGetActivationFactory((HSTRING)get_abi(name), (IActivationFactory**)put_abi(object));

			check_hresult(hr);

			return object;
		}

		template <typename Class>
		struct factory_local_entry
		{
			template <typename F>
			auto call(F&& callback)
			{
				{
					count_guard const guard(m_value.count);

					if (m_value.object)
					{
						return callback(*reinterpret_cast<com_ref<Windows::Foundation::IActivationFactory> const*>(&m_value.object));
					}
				}

				auto object = get_local_activation_factory<Class>();

				if (!object.template try_as<IAgileObject>())
				{
					return callback(object);
				}

				{
					count_guard const guard(m_value.count);

					if (nullptr == _InterlockedCompareExchangePointer((void**)& m_value.object, get_abi(object), nullptr))
					{
						// This thread successfully updated the entry to hold the factory object. We thus detach, since the
						// factory_cache_entry now owns the reference, and add the entry to the cache list. The callback
						// may be safely called using the cached object since the count guard is currently being held.
						detach_abi(object);
						get_factory_cache().add(reinterpret_cast<factory_cache_typeless_entry*>(this));
						return callback(*reinterpret_cast<com_ref<Windows::Foundation::IActivationFactory> const*>(&m_value.object));
					}
					else
					{
						// This thread failed to update the entry since another thread managed to exchange pointers first.
						// The callback must still be called and can simply use the temporary factory object before allowing
						// it to be released. 
						return callback(object);
					}
				}
			}

		private:

			struct count_guard
			{
				count_guard(count_guard const&) = delete;
				count_guard& operator=(count_guard const&) = delete;

				explicit count_guard(size_t& count) noexcept : m_count(count)
				{
#ifdef _WIN64
					_InterlockedIncrement64((int64_t*)& m_count);
#else
					_InterlockedIncrement((long*)& m_count);
#endif
				}

				~count_guard() noexcept
				{
#ifdef _WIN64
					_InterlockedDecrement64((int64_t*)& m_count);
#else
					_InterlockedDecrement((long*)& m_count);
#endif
				}

			private:

				size_t& m_count;
			};

			struct alignas(sizeof(void*) * 2) object_and_count
			{
				void* object;
				size_t count;
			};

			object_and_count m_value;
			alignas(memory_allocation_alignment) slist_entry m_next;
		};

		#define MAKE_LOCAL_AF(x) \
			template <> \
			struct factory_cache_entry<Microsoft::Graphics::Canvas::Effects::x, Windows::Foundation::IActivationFactory> : factory_local_entry<Microsoft::Graphics::Canvas::Effects::x> {};

		MAKE_LOCAL_AF(ColorSourceEffect);
		MAKE_LOCAL_AF(Transform2DEffect);
		MAKE_LOCAL_AF(ColorMatrixEffect);
		MAKE_LOCAL_AF(CompositeEffect);
	}
}

#include "winrt/Microsoft.Graphics.Canvas.Effects.h"

static class DPIScaler
{
public:
	DPIScaler()
	{
		// Default DPI is 96 (100%)
		dpiX = 96;
		dpiY = 96;
	}

	void SetScale(UINT dpiX, UINT dpiY)
	{
		this->dpiX = dpiX;
		this->dpiY = dpiY;
	}

	int ScaleX(int x)
	{
		return MulDiv(x, dpiX, 96);
	}

	int ScaleY(int y)
	{
		return MulDiv(y, dpiY, 96);
	}

private:
	UINT dpiX, dpiY;
} g_dpi;

using namespace winrt::Windows::UI;
using namespace winrt::Windows::UI::Composition;
using namespace winrt::Windows::UI::Xaml::Hosting;
using namespace winrt::Windows::Foundation::Numerics;

struct TenUI
{
	DesktopWindowXamlSource uiSource{ nullptr };

	winrt::Windows::UI::Xaml::Controls::TextBlock topStatic{ nullptr };
	winrt::Windows::UI::Xaml::Controls::TextBlock bottomStatic{ nullptr };
	winrt::Windows::UI::Xaml::Controls::ProgressBar progressBar{ nullptr };
};

//static thread_local struct  
static struct
{
	HWND rootWindow;
	HWND topStatic;
	HWND bottomStatic;
	HWND progressBar;
	HWND cancelButton;

	HWND tenWindow;

	UINT taskbarMsg;

	bool tenMode;
	bool canceled;

	std::unique_ptr<TenUI> ten;

	ITaskbarList3* tbList;

	wchar_t topText[512];
	wchar_t bottomText[512];
} g_uui;

HWND UI_GetWindowHandle()
{
	return g_uui.rootWindow;
}

HFONT UI_CreateScaledFont(int cHeight, int cWidth, int cEscapement, int cOrientation, int cWeight, DWORD bItalic,
	DWORD bUnderline, DWORD bStrikeOut, DWORD iCharSet, DWORD iOutPrecision, DWORD iClipPrecision,
	DWORD iQuality, DWORD iPitchAndFamily, LPCWSTR pszFaceName)
{
	LOGFONT logFont;
	
	memset(&logFont, 0, sizeof(LOGFONT));
	logFont.lfHeight = g_dpi.ScaleY(cHeight);
	logFont.lfWidth = cWidth;
	logFont.lfEscapement = cEscapement;
	logFont.lfOrientation = cOrientation;
	logFont.lfWeight = cWeight;
	logFont.lfItalic = bItalic;
	logFont.lfUnderline = bUnderline;
	logFont.lfStrikeOut = bStrikeOut;
	logFont.lfCharSet = iCharSet;
	logFont.lfOutPrecision = 8;
	logFont.lfClipPrecision = iClipPrecision;
	logFont.lfQuality = iQuality;
	logFont.lfPitchAndFamily = iPitchAndFamily;
	wcscpy_s(logFont.lfFaceName, pszFaceName);
	return CreateFontIndirect(&logFont);
}

static std::wstring g_mainXaml = LR"(
<Grid
    xmlns="http://schemas.microsoft.com/winfx/2006/xaml/presentation"
    xmlns:x="http://schemas.microsoft.com/winfx/2006/xaml"
    xmlns:d="http://schemas.microsoft.com/expression/blend/2008"
    xmlns:mc="http://schemas.openxmlformats.org/markup-compatibility/2006"
	xmlns:local="using:CitiLaunch"
    mc:Ignorable="d">

    <Grid Width="525" Height="525">
        <Grid.Resources>
            <!--<ThemeShadow x:Name="SharedShadow">
            </ThemeShadow>-->
        </Grid.Resources>
        <Grid x:Name="BackdropGrid" />
		<SwapChainPanel x:Name="Overlay" />
        <StackPanel Orientation="Vertical" VerticalAlignment="Center">)"
#if defined(GTA_FIVE)
	R"(
            <Viewbox Height="150" Margin="0,0,0,15" RenderTransformOrigin="0.5,0.5">
                <Path Data="F1 M 0,0 L 43.57,0 C 44.53,0 47.41,-9.44 52.22,-28.18 68.71,-85.82 78.32,-119.44 80.73,-129.21
        L 52.54,-156.91 51.74,-156.91 C 48.05,-145.22 30.43,-93.02 -0.8,-0.48 L 0,0 0,0 z
        M 83.93,-141.06 L 84.41,-141.06 C 84.89,-143.46 85.21,-144.74 85.21,-145.22 L 85.21,-146.02 C 77.04,-154.51 67.91,-163.64 57.82,-173.4
            56.86,-171.64 56.22,-170.36 56.22,-169.24 L 56.22,-168.76 C 66.47,-158.35 75.6,-149.07 83.93,-141.06 z
        M 136.94,-109.2 L 137.42,-109.2 C 131.82,-126.97 128.45,-136.1 127.17,-136.58 L 65.99,-197.26 C 65.35,-197.26 63.91,-192.94 61.51,-184.29
        L 136.94,-109.2 z
        M 125.57,-142.66 L 125.89,-142.66 C 113.4,-180.61 106.83,-199.82 106.03,-200.14 L 68.39,-200.14 68.39,-199.82
        C 82.33,-185.57 101.39,-166.68 125.57,-142.66 z
        M 147.99,-77.01 L 148.47,-77.01 C 147.03,-83.74 143.83,-88.86 138.54,-92.54 122.69,-108.88 106.83,-124.73 90.98,-140.26
        L 90.5,-140.26 C 91.46,-134.65 93.7,-130.49 97.06,-127.61 L 147.99,-77.01 z
        M 173.62,0 L 174.58,-0.48 C 162.89,-35.22 156.64,-53.16 155.68,-54.28 L 99.46,-110.16 99.46,-109.68
        C 101.55,-101.19 112.12,-64.52 130.86,0 L 173.62,0 173.62,0 z" Fill="#f40552" Stretch="Fill">
                </Path>
                <Viewbox.RenderTransform>
                    <ScaleTransform ScaleX="-1" />
                </Viewbox.RenderTransform>)"
#elif defined(IS_RDR3)
	R"(
			<Viewbox Height="150" Margin="0,0,0,15">
				<Grid>
				<Path Data="F1 M 38.56,38.56 L 779.52,38.56 779.52,1019.52 38.56,1019.52 z"  Fill="#00000000" />
				<Path Data="F1 M 153.23,78.44 L 154.67,77.16 153.23,75.72 153.23,78.44 153.23,78.44 z"  Fill="#ffffffff" />
				<Path Data="F1 M 677.12,48.82 L 523.2,98.61 516.32,118.63 673.43,67.71 677.12,48.82 677.12,48.82 z"  Fill="#ffffffff" />
				<Path Data="F1 M 666.07,105.5 L 668.63,92.37 507.35,144.73 502.7,158.34 666.07,105.5 666.07,105.5 z"  Fill="#ffffffff" />
				<Path Data="F1 M 0,0 L -13.94,40.99 -32.52,105.83 -42.61,153.07 116.91,176.77 134.69,107.28 166.24,-53.8
					 0,0 0.16,0 z" RenderTransform="1,0,0,1,496.62,175.63" Fill="#ffffffff" />
				<Path Data="F1 M 670.55,38.73 L 543.7,38.73 527.85,84.84 670.55,38.73 z"  Fill="#ffffffff" />
				<Path Data="F1 M 311.47,167.46 L 152.43,218.86 151.95,224.46 151.95,236.79 310.51,185.55 311.47,167.46 z"  Fill="#ffffffff" />
				<Path Data="F1 M 308.91,221.26 L 309.55,209.09 151.95,260.01 151.95,272.18 308.91,221.26 308.91,221.26 z"  Fill="#ffffffff" />
				<Path Data="F1 M 0,0 L -9.45,-0.96 19.22,-146.18 -133.89,-168.92 -144.78,-118.16 -164.64,-6.72 -266.82,-6.72
					 -245.52,-296.05 -245.52,-404.93 -244.72,-425.59 -401.04,-375.15 -401.04,-265.63 -405.2,-256.34 -404.88,-247.7
					 -409.05,-182.05 -412.09,-168.76 -411.77,-133.06 -411.77,358.34 94.18,358.34 94.18,326.48 116.76,-5.28
					 34.44,0 0,0 0,0 z
					M -25.14,211.04 L -272.27,211.04 -264.26,171.33 -264.26,143.31 -272.27,124.73 -25.14,124.73 -27.87,163.16
					 -25.14,211.04 z" RenderTransform="1,0,0,1,552.99,662.7" Fill="#ffffffff" />
				<Path Data="F1 M 0,0 L 2.88,-108.56 -156.31,-113.84 -155.83,-101.99 -157.12,-79.41 -157.12,37.47 -158.24,51.08
					 0,0 0,0 z" RenderTransform="1,0,0,1,311.79,155.13" Fill="#ffffffff" />
				</Grid>
)"
#endif
R"(         </Viewbox>
            <TextBlock x:Name="static1" Text=" " TextAlignment="Center" Foreground="#ffffffff" FontSize="24" />
			<Grid Margin="0,15,0,15">
				<ProgressBar x:Name="progressBar" Foreground="White" Width="250" />
			</Grid>
            <TextBlock x:Name="static2" Text=" " TextAlignment="Center" Foreground="#ffeeeeee" FontSize="18" />
        </StackPanel>
    </Grid>
</Grid>
)";

struct BackdropBrush : winrt::CitiLaunch::implementation::BackdropBrushT<BackdropBrush>
{
	BackdropBrush() = default;

	void OnConnected();
	void OnDisconnected();

	winrt::Windows::UI::Composition::CompositionPropertySet ps{ nullptr };
};

void BackdropBrush::OnConnected()
{
	if (!CompositionBrush())
	{
		auto effect = winrt::Microsoft::Graphics::Canvas::Effects::ColorSourceEffect();

#ifdef GTA_FIVE
		effect.Color(winrt::Windows::UI::ColorHelper::FromArgb(255, 0x16, 0x19, 0x23));
#elif defined(IS_RDR3)
		effect.Color(winrt::Windows::UI::ColorHelper::FromArgb(255, 186, 2, 2));
#endif

		winrt::Windows::UI::Composition::CompositionEffectSourceParameter sp{ L"layer" };
		winrt::Windows::UI::Composition::CompositionEffectSourceParameter sp2{ L"rawImage" };

		auto mat2d = winrt::Windows::Foundation::Numerics::float3x2{};

		using namespace DirectX;
		auto matrix = XMMatrixTransformation2D(XMVectorSet(0.0f, 0.0f, 0.0f, 0.0f), 0.0f, XMVectorSet(1.0f, 1.0f, 1.0f, 1.0f), XMVectorSet(0.5f, 0.5f, 0.0f, 0.0f), 0.2, XMVectorSet(0.0f, 0.0f, 0.0f, 1.0f));
		XMStoreFloat3x2(&mat2d, matrix);

		auto layer = winrt::Microsoft::Graphics::Canvas::Effects::Transform2DEffect();
		layer.Source(sp2);
		layer.Name(L"xform");
		layer.TransformMatrix(mat2d);

		auto mat = winrt::Microsoft::Graphics::Canvas::Effects::Matrix5x4();
		memset(&mat, 0, sizeof(mat));
		mat.M44 = 1.0f;

#ifdef GTA_FIVE
		mat.M11 = 1.0f;
		mat.M22 = 1.0f;
		mat.M33 = 1.0f;
		mat.M44 = 0.03f;
#elif defined(IS_RDR3)
		mat.M11 = 1.0f;
		mat.M22 = 1.0f;
		mat.M33 = 1.0f;
		mat.M44 = 0.15f;
#endif

		auto layerColor = winrt::Microsoft::Graphics::Canvas::Effects::ColorMatrixEffect();
		layerColor.Source(layer);
		layerColor.ColorMatrix(mat);

		auto compEffect = winrt::Microsoft::Graphics::Canvas::Effects::CompositeEffect();
		compEffect.Sources().Append(effect);
		compEffect.Sources().Append(layerColor);

		auto hRsc = FindResource(GetModuleHandle(NULL), MAKEINTRESOURCE(1010), L"MEOW");
		auto resSize = SizeofResource(GetModuleHandle(NULL), hRsc);
		auto resData = LoadResource(GetModuleHandle(NULL), hRsc);

		auto resPtr = static_cast<const uint8_t*>(LockResource(resData));

		auto iras = winrt::Windows::Storage::Streams::InMemoryRandomAccessStream();
		auto dw = winrt::Windows::Storage::Streams::DataWriter{ iras };
		dw.WriteBytes(winrt::array_view<const uint8_t>{resPtr, resPtr + resSize});

		auto iao = dw.StoreAsync();
		while (iao.Status() != winrt::Windows::Foundation::AsyncStatus::Completed)
		{
			Sleep(0);
		}

		iras.Seek(0);

		auto surf = winrt::Windows::UI::Xaml::Media::LoadedImageSurface::StartLoadFromStream(iras);

		auto cb = winrt::Windows::UI::Xaml::Window::Current().Compositor().CreateSurfaceBrush();
		cb.Surface(surf);
		//cb.Stretch(winrt::Windows::UI::Composition::CompositionStretch::UniformToFill);
		cb.Stretch(winrt::Windows::UI::Composition::CompositionStretch::None);

		auto ef = winrt::Windows::UI::Xaml::Window::Current().Compositor().CreateEffectFactory(compEffect, { L"xform.TransformMatrix" });
		auto eb = ef.CreateBrush();
		eb.SetSourceParameter(L"rawImage", cb);

		using namespace std::chrono_literals;

		auto kfa = winrt::Windows::UI::Xaml::Window::Current().Compositor().CreateVector2KeyFrameAnimation();
		kfa.InsertKeyFrame(0.0f, { 0.0f, 0.0f });
		kfa.InsertKeyFrame(0.25f, { 0.0f, -300.0f }, winrt::Windows::UI::Xaml::Window::Current().Compositor().CreateLinearEasingFunction());
		kfa.InsertKeyFrame(0.5f, { -300.0f, -300.0f }, winrt::Windows::UI::Xaml::Window::Current().Compositor().CreateLinearEasingFunction());
		kfa.InsertKeyFrame(0.75f, { -300.0f, 0.0f }, winrt::Windows::UI::Xaml::Window::Current().Compositor().CreateLinearEasingFunction());
		kfa.InsertKeyFrame(1.0f, { 0.0f, 0.0f }, winrt::Windows::UI::Xaml::Window::Current().Compositor().CreateLinearEasingFunction());
		kfa.Duration(60s);
		kfa.IterationBehavior(winrt::Windows::UI::Composition::AnimationIterationBehavior::Forever);
		kfa.Target(L"xlate");

		auto ag = winrt::Windows::UI::Xaml::Window::Current().Compositor().CreateAnimationGroup();
		ag.Add(kfa);

		ps = winrt::Windows::UI::Xaml::Window::Current().Compositor().CreatePropertySet();
		ps.InsertVector2(L"xlate", { 0.0f, 0.0f });
		ps.StartAnimationGroup(ag);

		auto ca = winrt::Windows::UI::Xaml::Window::Current().Compositor().CreateExpressionAnimation();
		ca.SetReferenceParameter(L"ps", ps);
		ca.SetMatrix3x2Parameter(L"rot", mat2d);
		ca.Expression(L"Matrix3x2.CreateFromTranslation(ps.xlate) * rot");

		eb.StartAnimation(L"xform.TransformMatrix", ca);

		CompositionBrush(eb);
	}
}

void BackdropBrush::OnDisconnected()
{
	if (CompositionBrush())
	{
		CompositionBrush(nullptr);
	}
}

#include <wrl.h>
#include <d3d11.h>
#include <dxgi1_4.h>

#include <windows.ui.xaml.media.dxinterop.h>

using Microsoft::WRL::ComPtr;

const BYTE g_PixyShader[] =
{
     68,  88,  66,  67, 115,  61, 
    165, 134, 202, 176,  67, 148, 
    204, 160, 214, 207, 231, 188, 
    224, 101,   1,   0,   0,   0, 
     48,  10,   0,   0,   5,   0, 
      0,   0,  52,   0,   0,   0, 
     36,   1,   0,   0, 124,   1, 
      0,   0, 176,   1,   0,   0, 
    180,   9,   0,   0,  82,  68, 
     69,  70, 232,   0,   0,   0, 
      1,   0,   0,   0,  68,   0, 
      0,   0,   1,   0,   0,   0, 
     28,   0,   0,   0,   0,   4, 
    255, 255,   0,   1,   0,   0, 
    192,   0,   0,   0,  60,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   1,   0, 
      0,   0,   1,   0,   0,   0, 
     80, 115,  67,  98, 117, 102, 
      0, 171,  60,   0,   0,   0, 
      2,   0,   0,   0,  92,   0, 
      0,   0,  16,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0, 140,   0,   0,   0, 
      0,   0,   0,   0,   8,   0, 
      0,   0,   0,   0,   0,   0, 
    152,   0,   0,   0,   0,   0, 
      0,   0, 168,   0,   0,   0, 
      8,   0,   0,   0,   4,   0, 
      0,   0,   2,   0,   0,   0, 
    176,   0,   0,   0,   0,   0, 
      0,   0, 105,  82, 101, 115, 
    111, 108, 117, 116, 105, 111, 
    110,   0,   1,   0,   3,   0, 
      1,   0,   2,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
    105,  84, 105, 109, 101,   0, 
    171, 171,   0,   0,   3,   0, 
      1,   0,   1,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
     77, 105,  99, 114, 111, 115, 
    111, 102, 116,  32,  40,  82, 
     41,  32,  72,  76,  83,  76, 
     32,  83, 104,  97, 100, 101, 
    114,  32,  67, 111, 109, 112, 
    105, 108, 101, 114,  32,  49, 
     48,  46,  49,   0,  73,  83, 
     71,  78,  80,   0,   0,   0, 
      2,   0,   0,   0,   8,   0, 
      0,   0,  56,   0,   0,   0, 
      0,   0,   0,   0,   1,   0, 
      0,   0,   3,   0,   0,   0, 
      0,   0,   0,   0,  15,   0, 
      0,   0,  68,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   3,   0,   0,   0, 
      1,   0,   0,   0,   3,   3, 
      0,   0,  83,  86,  95,  80, 
     79,  83,  73,  84,  73,  79, 
     78,   0,  84,  69,  88,  67, 
     79,  79,  82,  68,   0, 171, 
    171, 171,  79,  83,  71,  78, 
     44,   0,   0,   0,   1,   0, 
      0,   0,   8,   0,   0,   0, 
     32,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      3,   0,   0,   0,   0,   0, 
      0,   0,  15,   0,   0,   0, 
     83,  86,  95,  84,  65,  82, 
     71,  69,  84,   0, 171, 171, 
     83,  72,  68,  82, 252,   7, 
      0,   0,  64,   0,   0,   0, 
    255,   1,   0,   0,  89,   0, 
      0,   4,  70, 142,  32,   0, 
      0,   0,   0,   0,   1,   0, 
      0,   0,  98,  16,   0,   3, 
     50,  16,  16,   0,   1,   0, 
      0,   0, 101,   0,   0,   3, 
    242,  32,  16,   0,   0,   0, 
      0,   0, 104,   0,   0,   2, 
      5,   0,   0,   0,  56,   0, 
      0,  11,  50,   0,  16,   0, 
      0,   0,   0,   0, 166, 138, 
     32,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   2,  64, 
      0,   0, 205, 204, 204,  61, 
     66,  96, 101,  63,   0,   0, 
      0,   0,   0,   0,   0,   0, 
     77,   0,   0,   6,  18,   0, 
     16,   0,   0,   0,   0,   0, 
      0, 208,   0,   0,  10,   0, 
     16,   0,   0,   0,   0,   0, 
     50,   0,   0,  15, 194,   0, 
     16,   0,   0,   0,   0,   0, 
     86,  17,  16,   0,   1,   0, 
      0,   0,   2,  64,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0, 128, 191, 
      0,   0, 128,  63,   2,  64, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
    128,  63,   0,   0,   0,   0, 
     54,   0,   0,   8,  50,   0, 
     16,   0,   1,   0,   0,   0, 
      2,  64,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,  48,   0,   0,   1, 
     33,   0,   0,   7,  66,   0, 
     16,   0,   1,   0,   0,   0, 
     26,   0,  16,   0,   1,   0, 
      0,   0,   1,  64,   0,   0, 
     66,   0,   0,   0,   3,   0, 
      4,   3,  42,   0,  16,   0, 
      1,   0,   0,   0,  43,   0, 
      0,   5,  66,   0,  16,   0, 
      1,   0,   0,   0,  26,   0, 
     16,   0,   1,   0,   0,   0, 
     56,   0,   0,   7, 130,   0, 
     16,   0,   1,   0,   0,   0, 
     42,   0,  16,   0,   1,   0, 
      0,   0,   1,  64,   0,   0, 
     53, 165, 231,  64,  50,   0, 
      0,  15, 114,   0,  16,   0, 
      2,   0,   0,   0, 166,  10, 
     16,   0,   1,   0,   0,   0, 
      2,  64,   0,   0,   0,   0, 
      0,  63, 143, 194, 117,  60, 
     10, 215,  35,  60,   0,   0, 
      0,   0,   2,  64,   0,   0, 
      0,   0, 128,  63,   0,   0, 
    128,  63,   0,   0, 128,  63, 
      0,   0,   0,   0,  56,   0, 
      0,   7, 130,   0,  16,   0, 
      2,   0,   0,   0,  42,   0, 
     16,   0,   0,   0,   0,   0, 
     10,   0,  16,   0,   2,   0, 
      0,   0,  65,   0,   0,   5, 
    130,   0,  16,   0,   1,   0, 
      0,   0,  58,   0,  16,   0, 
      1,   0,   0,   0,  50,   0, 
      0,  10, 130,   0,  16,   0, 
      1,   0,   0,   0,  42,   0, 
     16,   0,   1,   0,   0,   0, 
      1,  64,   0,   0,  53, 165, 
    231,  64,  58,   0,  16, 128, 
     65,   0,   0,   0,   1,   0, 
      0,   0,  50,   0,   0,  10, 
     18,   0,  16,   0,   3,   0, 
      0,   0,  42, 128,  32,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   1,  64,   0,   0, 
      0,   0,   0,  64,  42,   0, 
     16,   0,   1,   0,   0,   0, 
     77,   0,   0,   6,  18,   0, 
     16,   0,   3,   0,   0,   0, 
      0, 208,   0,   0,  10,   0, 
     16,   0,   3,   0,   0,   0, 
     50,   0,   0,  10, 130,   0, 
     16,   0,   1,   0,   0,   0, 
     10,   0,  16, 128,  65,   0, 
      0,   0,   3,   0,   0,   0, 
      1,  64,   0,   0, 205, 204, 
    204,  61,  58,   0,  16,   0, 
      1,   0,   0,   0,  56,   0, 
      0,   7,  34,   0,  16,   0, 
      3,   0,   0,   0,  58,   0, 
     16,   0,   1,   0,   0,   0, 
     58,   0,  16,   0,   2,   0, 
      0,   0,  14,   0,   0,   7, 
     18,   0,  16,   0,   3,   0, 
      0,   0,  26,   0,  16,   0, 
      0,   0,   0,   0,  26,   0, 
     16,   0,   2,   0,   0,   0, 
     50,   0,   0,   9,  50,   0, 
     16,   0,   2,   0,   0,   0, 
    230,  10,  16,   0,   0,   0, 
      0,   0,   6,   0,  16,   0, 
      2,   0,   0,   0,  70,   0, 
     16,   0,   3,   0,   0,   0, 
     65,   0,   0,   5,  50,   0, 
     16,   0,   3,   0,   0,   0, 
     22,   5,  16,   0,   2,   0, 
      0,   0,   0,   0,   0,   7, 
    130,   0,  16,   0,   1,   0, 
      0,   0,  42,   0,  16,   0, 
      1,   0,   0,   0,   1,  64, 
      0,   0,  18, 131, 249,  65, 
     65,   0,   0,   5,  66,   0, 
     16,   0,   3,   0,   0,   0, 
     58,   0,  16,   0,   1,   0, 
      0,   0,  50,   0,   0,  15, 
    114,   0,  16,   0,   4,   0, 
      0,   0,  70,   2,  16,   0, 
      3,   0,   0,   0,   2,  64, 
      0,   0, 172, 197,  39,  55, 
    172, 197,  39,  55, 172, 197, 
     39,  55,   0,   0,   0,   0, 
      2,  64,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0, 137,  65,  62,   0,   0, 
      0,   0,  50,   0,   0,  15, 
    194,   0,  16,   0,   3,   0, 
      0,   0,   6,   4,  16,   0, 
      3,   0,   0,   0,   2,  64, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0, 172, 197, 
     39,  55, 172, 197,  39,  55, 
      2,  64,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
    205, 111, 245,  70, 205, 111, 
    245,  70,  16,   0,   0,  10, 
    130,   0,  16,   0,   1,   0, 
      0,   0,   2,  64,   0,   0, 
    130,  43,  85,  65, 240,  22, 
    188,  65, 153, 176, 173,  65, 
      0,   0,   0,   0,  70,   2, 
     16,   0,   4,   0,   0,   0, 
     16,   0,   0,  10, 130,   0, 
     16,   0,   2,   0,   0,   0, 
      2,  64,   0,   0,  56, 248, 
    168,  65, 127, 217, 229,  65, 
     50, 230,  62,  65,   0,   0, 
      0,   0,  70,   2,  16,   0, 
      4,   0,   0,   0,  26,   0, 
      0,   5,  18,   0,  16,   0, 
      4,   0,   0,   0,  58,   0, 
     16,   0,   1,   0,   0,   0, 
     26,   0,   0,   5,  34,   0, 
     16,   0,   4,   0,   0,   0, 
     58,   0,  16,   0,   2,   0, 
      0,   0,  14,   0,   0,   7, 
    194,   0,  16,   0,   3,   0, 
      0,   0, 166,  14,  16,   0, 
      3,   0,   0,   0,   6,   4, 
     16,   0,   4,   0,   0,   0, 
     26,   0,   0,   5, 194,   0, 
     16,   0,   3,   0,   0,   0, 
    166,  14,  16,   0,   3,   0, 
      0,   0,   0,   0,   0,   8, 
     50,   0,  16,   0,   3,   0, 
      0,   0,  22,   5,  16,   0, 
      2,   0,   0,   0,  70,   0, 
     16, 128,  65,   0,   0,   0, 
      3,   0,   0,   0,   0,   0, 
      0,  10,  50,   0,  16,   0, 
      3,   0,   0,   0,  70,   0, 
     16,   0,   3,   0,   0,   0, 
      2,  64,   0,   0,   0,   0, 
      0, 191,   0,   0,   0, 191, 
      0,   0,   0,   0,   0,   0, 
      0,   0,  50,   0,   0,  12, 
     50,   0,  16,   0,   3,   0, 
      0,   0, 230,  10,  16,   0, 
      3,   0,   0,   0,   2,  64, 
      0,   0, 102, 102, 102,  63, 
    102, 102, 102,  63,   0,   0, 
      0,   0,   0,   0,   0,   0, 
     70,   0,  16,   0,   3,   0, 
      0,   0,   0,   0,   0,  10, 
     50,   0,  16,   0,   3,   0, 
      0,   0,  70,   0,  16,   0, 
      3,   0,   0,   0,   2,  64, 
      0,   0, 102, 102, 230, 190, 
    102, 102, 230, 190,   0,   0, 
      0,   0,   0,   0,   0,   0, 
     56,   0,   0,  10,  50,   0, 
     16,   0,   2,   0,   0,   0, 
     70,   0,  16,   0,   2,   0, 
      0,   0,   2,  64,   0,   0, 
      0,   0,  32,  65,   0,   0, 
     32,  65,   0,   0,   0,   0, 
      0,   0,   0,   0,  26,   0, 
      0,   5,  50,   0,  16,   0, 
      2,   0,   0,   0,  70,   0, 
     16,   0,   2,   0,   0,   0, 
     50,   0,   0,  15,  50,   0, 
     16,   0,   2,   0,   0,   0, 
     70,   0,  16,   0,   2,   0, 
      0,   0,   2,  64,   0,   0, 
      0,   0,   0,  64,   0,   0, 
      0,  64,   0,   0,   0,   0, 
      0,   0,   0,   0,   2,  64, 
      0,   0,   0,   0, 128, 191, 
      0,   0, 128, 191,   0,   0, 
      0,   0,   0,   0,   0,   0, 
     50,   0,   0,  14,  50,   0, 
     16,   0,   2,   0,   0,   0, 
     70,   0,  16, 128, 129,   0, 
      0,   0,   2,   0,   0,   0, 
      2,  64,   0,   0,  10, 215, 
     35,  60,  10, 215,  35,  60, 
      0,   0,   0,   0,   0,   0, 
      0,   0,  70,   0,  16, 128, 
    129,   0,   0,   0,   3,   0, 
      0,   0,   0,   0,   0,   8, 
    130,   0,  16,   0,   1,   0, 
      0,   0,  26,   0,  16, 128, 
     65,   0,   0,   0,   2,   0, 
      0,   0,  10,   0,  16,   0, 
      2,   0,   0,   0,   0,   0, 
      0,   7, 130,   0,  16,   0, 
      2,   0,   0,   0,  26,   0, 
     16,   0,   2,   0,   0,   0, 
     10,   0,  16,   0,   2,   0, 
      0,   0,  52,   0,   0,   7, 
    130,   0,  16,   0,   1,   0, 
      0,   0,  58,   0,  16,   0, 
      1,   0,   0,   0,  58,   0, 
     16,   0,   2,   0,   0,   0, 
     52,   0,   0,   7,  18,   0, 
     16,   0,   2,   0,   0,   0, 
     26,   0,  16,   0,   2,   0, 
      0,   0,  10,   0,  16,   0, 
      2,   0,   0,   0,  50,   0, 
      0,   9, 130,   0,  16,   0, 
      1,   0,   0,   0,  58,   0, 
     16,   0,   1,   0,   0,   0, 
      1,  64,   0,   0, 154, 153, 
     25,  63,  10,   0,  16,   0, 
      2,   0,   0,   0,  50,   0, 
      0,  10,  66,   0,  16,   0, 
      1,   0,   0,   0,  10,   0, 
     16, 128,  65,   0,   0,   0, 
      0,   0,   0,   0,   1,  64, 
      0,   0,   0,   0, 160,  64, 
     42,   0,  16,   0,   1,   0, 
      0,   0,   0,   0,   0,  10, 
    194,   0,  16,   0,   1,   0, 
      0,   0, 166,  14,  16,   0, 
      1,   0,   0,   0,   2,  64, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
    160, 192,  10, 215,  35, 188, 
     56,   0,   0,   8,  66,   0, 
     16,   0,   1,   0,   0,   0, 
     42,   0,  16, 128, 129,   0, 
      0,   0,   1,   0,   0,   0, 
      1,  64,   0,   0,   0,   0, 
      0,  63,  51,   0,   0,   7, 
     66,   0,  16,   0,   1,   0, 
      0,   0,  42,   0,  16,   0, 
      1,   0,   0,   0,   1,  64, 
      0,   0,   0,   0, 128,  63, 
     50,   0,   0,   9,  66,   0, 
     16,   0,   1,   0,   0,   0, 
     42,   0,  16,   0,   1,   0, 
      0,   0,   1,  64,   0,   0, 
    205, 204,  76,  61,   1,  64, 
      0,   0, 205, 204,  76,  61, 
     56,   0,   0,   7,  18,   0, 
     16,   0,   2,   0,   0,   0, 
     42,   0,  16,   0,   1,   0, 
      0,   0,   1,  64,   0,   0, 
      0,   0,   0, 192,   0,   0, 
      0,   8,  66,   0,  16,   0, 
      1,   0,   0,   0,  42,   0, 
     16, 128,  65,   0,   0,   0, 
      1,   0,   0,   0,  58,   0, 
     16,   0,   1,   0,   0,   0, 
     14,   0,   0,  10, 130,   0, 
     16,   0,   1,   0,   0,   0, 
      2,  64,   0,   0,   0,   0, 
    128,  63,   0,   0, 128,  63, 
      0,   0, 128,  63,   0,   0, 
    128,  63,  10,   0,  16,   0, 
      2,   0,   0,   0,  56,  32, 
      0,   7,  66,   0,  16,   0, 
      1,   0,   0,   0,  58,   0, 
     16,   0,   1,   0,   0,   0, 
     42,   0,  16,   0,   1,   0, 
      0,   0,  50,   0,   0,   9, 
    130,   0,  16,   0,   1,   0, 
      0,   0,  42,   0,  16,   0, 
      1,   0,   0,   0,   1,  64, 
      0,   0,   0,   0,   0, 192, 
      1,  64,   0,   0,   0,   0, 
     64,  64,  56,   0,   0,   7, 
     66,   0,  16,   0,   1,   0, 
      0,   0,  42,   0,  16,   0, 
      1,   0,   0,   0,  42,   0, 
     16,   0,   1,   0,   0,   0, 
     56,   0,   0,   7,  66,   0, 
     16,   0,   1,   0,   0,   0, 
     42,   0,  16,   0,   1,   0, 
      0,   0,  58,   0,  16,   0, 
      1,   0,   0,   0,  14,   0, 
      0,   7, 130,   0,  16,   0, 
      1,   0,   0,   0,  42,   0, 
     16,   0,   3,   0,   0,   0, 
     42,   0,  16,   0,   2,   0, 
      0,   0,  50,   0,   0,   9, 
     18,   0,  16,   0,   1,   0, 
      0,   0,  42,   0,  16,   0, 
      1,   0,   0,   0,  58,   0, 
     16,   0,   1,   0,   0,   0, 
     10,   0,  16,   0,   1,   0, 
      0,   0,  30,   0,   0,   7, 
     34,   0,  16,   0,   1,   0, 
      0,   0,  26,   0,  16,   0, 
      1,   0,   0,   0,   1,  64, 
      0,   0,   1,   0,   0,   0, 
     22,   0,   0,   1,  54,   0, 
      0,   5, 130,  32,  16,   0, 
      0,   0,   0,   0,  10,   0, 
     16,   0,   1,   0,   0,   0, 
     54,   0,   0,   8, 114,  32, 
     16,   0,   0,   0,   0,   0, 
      2,  64,   0,   0,   0,   0, 
    128,  63,   0,   0, 128,  63, 
      0,   0, 128,  63,   0,   0, 
      0,   0,  62,   0,   0,   1, 
     83,  84,  65,  84, 116,   0, 
      0,   0,  62,   0,   0,   0, 
      5,   0,   0,   0,   0,   0, 
      0,   0,   2,   0,   0,   0, 
     52,   0,   0,   0,   2,   0, 
      0,   0,   0,   0,   0,   0, 
      1,   0,   0,   0,   1,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      3,   0,   0,   0,   0,   0, 
      0,   0,   8,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0,   0,   0, 
      0,   0,   0,   0
};

const BYTE g_VertyShader[] = {
	68, 88, 66, 67, 76, 75,
	108, 163, 29, 221, 215, 151,
	233, 28, 62, 114, 145, 31,
	52, 111, 1, 0, 0, 0,
	176, 2, 0, 0, 5, 0,
	0, 0, 52, 0, 0, 0,
	128, 0, 0, 0, 180, 0,
	0, 0, 12, 1, 0, 0,
	52, 2, 0, 0, 82, 68,
	69, 70, 68, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	28, 0, 0, 0, 0, 4,
	254, 255, 0, 1, 0, 0,
	28, 0, 0, 0, 77, 105,
	99, 114, 111, 115, 111, 102,
	116, 32, 40, 82, 41, 32,
	72, 76, 83, 76, 32, 83,
	104, 97, 100, 101, 114, 32,
	67, 111, 109, 112, 105, 108,
	101, 114, 32, 49, 48, 46,
	49, 0, 73, 83, 71, 78,
	44, 0, 0, 0, 1, 0,
	0, 0, 8, 0, 0, 0,
	32, 0, 0, 0, 0, 0,
	0, 0, 6, 0, 0, 0,
	1, 0, 0, 0, 0, 0,
	0, 0, 1, 1, 0, 0,
	83, 86, 95, 86, 69, 82,
	84, 69, 88, 73, 68, 0,
	79, 83, 71, 78, 80, 0,
	0, 0, 2, 0, 0, 0,
	8, 0, 0, 0, 56, 0,
	0, 0, 0, 0, 0, 0,
	1, 0, 0, 0, 3, 0,
	0, 0, 0, 0, 0, 0,
	15, 0, 0, 0, 68, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 3, 0,
	0, 0, 1, 0, 0, 0,
	3, 12, 0, 0, 83, 86,
	95, 80, 79, 83, 73, 84,
	73, 79, 78, 0, 84, 69,
	88, 67, 79, 79, 82, 68,
	0, 171, 171, 171, 83, 72,
	68, 82, 32, 1, 0, 0,
	64, 0, 1, 0, 72, 0,
	0, 0, 96, 0, 0, 4,
	18, 16, 16, 0, 0, 0,
	0, 0, 6, 0, 0, 0,
	103, 0, 0, 4, 242, 32,
	16, 0, 0, 0, 0, 0,
	1, 0, 0, 0, 101, 0,
	0, 3, 50, 32, 16, 0,
	1, 0, 0, 0, 104, 0,
	0, 2, 1, 0, 0, 0,
	54, 0, 0, 8, 194, 32,
	16, 0, 0, 0, 0, 0,
	2, 64, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	128, 63, 1, 0, 0, 7,
	18, 0, 16, 0, 0, 0,
	0, 0, 10, 16, 16, 0,
	0, 0, 0, 0, 1, 64,
	0, 0, 1, 0, 0, 0,
	85, 0, 0, 7, 66, 0,
	16, 0, 0, 0, 0, 0,
	10, 16, 16, 0, 0, 0,
	0, 0, 1, 64, 0, 0,
	1, 0, 0, 0, 86, 0,
	0, 5, 50, 0, 16, 0,
	0, 0, 0, 0, 134, 0,
	16, 0, 0, 0, 0, 0,
	0, 0, 0, 10, 194, 0,
	16, 0, 0, 0, 0, 0,
	6, 4, 16, 0, 0, 0,
	0, 0, 2, 64, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 191,
	0, 0, 0, 191, 54, 0,
	0, 5, 50, 32, 16, 0,
	1, 0, 0, 0, 70, 0,
	16, 0, 0, 0, 0, 0,
	0, 0, 0, 7, 18, 32,
	16, 0, 0, 0, 0, 0,
	42, 0, 16, 0, 0, 0,
	0, 0, 42, 0, 16, 0,
	0, 0, 0, 0, 56, 0,
	0, 7, 34, 32, 16, 0,
	0, 0, 0, 0, 58, 0,
	16, 0, 0, 0, 0, 0,
	1, 64, 0, 0, 0, 0,
	0, 192, 62, 0, 0, 1,
	83, 84, 65, 84, 116, 0,
	0, 0, 9, 0, 0, 0,
	1, 0, 0, 0, 0, 0,
	0, 0, 3, 0, 0, 0,
	3, 0, 0, 0, 0, 0,
	0, 0, 2, 0, 0, 0,
	1, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	2, 0, 0, 0, 0, 0,
	0, 0, 1, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0,
	0, 0, 0, 0
};

#include <mmsystem.h>

static void InitializeRenderOverlay(winrt::Windows::UI::Xaml::Controls::SwapChainPanel swapChainPanel, int w, int h)
{
	auto loadSystemDll = [](auto dll)
	{
		wchar_t systemPath[512];
		GetSystemDirectory(systemPath, _countof(systemPath));

		wcscat_s(systemPath, dll);

		return LoadLibrary(systemPath);
	};

	ComPtr<ID3D11Device> g_pd3dDevice = NULL;
	ComPtr<ID3D11DeviceContext> g_pd3dDeviceContext = NULL;
	ComPtr<IDXGISwapChain1> g_pSwapChain = NULL;
	ComPtr<ID3D11RenderTargetView> g_mainRenderTargetView = NULL;

	// Setup swap chain
	DXGI_SWAP_CHAIN_DESC1 sd;
	ZeroMemory(&sd, sizeof(sd));
	sd.BufferCount = 2;
	sd.Width = w;
	sd.Height = h;
	sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
	sd.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.SampleDesc.Count = 1;
	sd.SampleDesc.Quality = 0;
	sd.Scaling = DXGI_SCALING_STRETCH;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

	UINT createDeviceFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
	//createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG;
	D3D_FEATURE_LEVEL featureLevel;
	const D3D_FEATURE_LEVEL featureLevelArray[2] = {
		D3D_FEATURE_LEVEL_11_0,
		D3D_FEATURE_LEVEL_10_0,
	};

	auto d3d11 = loadSystemDll(L"\\d3d11.dll");
	auto _D3D11CreateDevice = (decltype(&D3D11CreateDevice))GetProcAddress(d3d11, "D3D11CreateDevice");

	if (_D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &g_pd3dDevice, &featureLevel, &g_pd3dDeviceContext) != S_OK)
	{
		return;
	}

	ComPtr<IDXGIDevice1> device1;
	if (FAILED(g_pd3dDevice.As(&device1)))
	{
		return;
	}

	ComPtr<IDXGIAdapter> adapter;
	if (FAILED(device1->GetAdapter(&adapter)))
	{
		return;
	}

	ComPtr<IDXGIFactory> parent;
	if (FAILED(adapter->GetParent(__uuidof(IDXGIFactory), &parent)))
	{
		return;
	}

	ComPtr<IDXGIFactory3> factory3;
	if (FAILED(parent.As(&factory3)))
	{
		return;
	}

	if (FAILED(factory3->CreateSwapChainForComposition(g_pd3dDevice.Get(), &sd, NULL, &g_pSwapChain)))
	{
		return;
	}

	{
		ComPtr<ID3D11Texture2D> pBackBuffer;
		g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
		g_pd3dDevice->CreateRenderTargetView(pBackBuffer.Get(), NULL, &g_mainRenderTargetView);
	}
	
	auto nativePanel = swapChainPanel.as<ISwapChainPanelNative>();
	nativePanel->SetSwapChain(g_pSwapChain.Get());

	std::thread([g_pd3dDevice, g_pd3dDeviceContext, g_pSwapChain, g_mainRenderTargetView, w, h]()
	{
		ComPtr<ID3D11VertexShader> vs;
		ComPtr<ID3D11PixelShader> ps;

		g_pd3dDevice->CreatePixelShader(g_PixyShader, sizeof(g_PixyShader), NULL, &ps);
		g_pd3dDevice->CreateVertexShader(g_VertyShader, sizeof(g_VertyShader), NULL, &vs);

		ComPtr<ID3D11BlendState> bs;

		{
			D3D11_BLEND_DESC desc = { 0 };
			desc.AlphaToCoverageEnable = false;
			desc.RenderTarget[0].BlendEnable = true;
			desc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
			desc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
			desc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
			desc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
			desc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ZERO;
			desc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
			desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;

			g_pd3dDevice->CreateBlendState(&desc, &bs);
		}

		struct CBuf
		{
			float res[2];
			float sec;
			float pad;
		};

		ComPtr<ID3D11Buffer> cbuf;

		{
			D3D11_BUFFER_DESC desc;
			desc.ByteWidth = sizeof(CBuf);
			desc.Usage = D3D11_USAGE_DYNAMIC;
			desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
			desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
			desc.MiscFlags = 0;
			g_pd3dDevice->CreateBuffer(&desc, NULL, &cbuf);
		}

		while (g_uui.ten)
		{
			// Setup viewport
			D3D11_VIEWPORT vp;
			memset(&vp, 0, sizeof(D3D11_VIEWPORT));
			vp.Width = w;
			vp.Height = h;
			vp.MinDepth = 0.0f;
			vp.MaxDepth = 1.0f;
			vp.TopLeftX = vp.TopLeftY = 0;
			g_pd3dDeviceContext->RSSetViewports(1, &vp);

			auto rtv = g_mainRenderTargetView.Get();
			g_pd3dDeviceContext->OMSetRenderTargets(1, &rtv, NULL);

			float clearColor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
			g_pd3dDeviceContext->ClearRenderTargetView(rtv, clearColor);

			g_pd3dDeviceContext->VSSetShader(vs.Get(), NULL, 0);
			g_pd3dDeviceContext->PSSetShader(ps.Get(), NULL, 0);

			auto cb = cbuf.Get();
			g_pd3dDeviceContext->VSSetConstantBuffers(0, 1, &cb);
			g_pd3dDeviceContext->PSSetConstantBuffers(0, 1, &cb);

			g_pd3dDeviceContext->OMSetBlendState(bs.Get(), NULL, 0xFFFFFFFF);

			static auto startTime = timeGetTime();

			D3D11_MAPPED_SUBRESOURCE mapped_resource;
			if (SUCCEEDED(g_pd3dDeviceContext->Map(cb, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped_resource)))
			{
				auto c = (CBuf*)mapped_resource.pData;
				c->res[0] = float(w);
				c->res[1] = float(h);
				c->sec = (timeGetTime() - startTime) / 1000.0f;
				g_pd3dDeviceContext->Unmap(cb, 0);
			}

			g_pd3dDeviceContext->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
			g_pd3dDeviceContext->Draw(4, 0);

			g_pSwapChain->Present(1, 0);
		}

		// prevent the thread from exiting (the CRT is broken and will crash on thread exit in some cases)
		WaitForSingleObject(GetCurrentProcess(), INFINITE);
	}).detach();
}

void UI_CreateWindow()
{
	g_uui.taskbarMsg = RegisterWindowMessage(L"TaskbarButtonCreated");

	HWND rootWindow = CreateWindowEx(0, L"NotSteamAtAll", PRODUCT_NAME, 13238272 /* lol */, 0x80000000, 0, g_dpi.ScaleX(500), g_dpi.ScaleY(129), NULL, NULL, GetModuleHandle(NULL), 0);

	int wwidth = 500;
	int wheight = 139;

	if (!g_uui.tenMode)
	{
		INITCOMMONCONTROLSEX controlSex;
		controlSex.dwSize = sizeof(controlSex);
		controlSex.dwICC = 16416; // lazy bum
		InitCommonControlsEx(&controlSex);

		HFONT font = UI_CreateScaledFont(-12, 0, 0, 0, 0, 0, 0, 0, 1, 8, 0, 5, 2, L"Tahoma");

		// TODO: figure out which static is placed where
		HWND static1 = CreateWindowEx(0x20, L"static", L"static1", 0x50000000, g_dpi.ScaleX(15), g_dpi.ScaleY(15), g_dpi.ScaleX(455), g_dpi.ScaleY(25), rootWindow, 0, GetModuleHandle(NULL) /* what?! */, 0);

		SendMessage(static1, WM_SETFONT, (WPARAM)font, 0);

		HWND cancelButton = CreateWindowEx(0, L"button", L"Cancel", 0x50000000, g_dpi.ScaleX(395), g_dpi.ScaleY(64), g_dpi.ScaleX(75), g_dpi.ScaleY(25), rootWindow, 0, GetModuleHandle(NULL), 0);
		SendMessage(cancelButton, WM_SETFONT, (WPARAM)font, 0);

		HWND progressBar = CreateWindowEx(0, L"msctls_progress32", 0, 0x50000000, g_dpi.ScaleX(15), g_dpi.ScaleY(40), g_dpi.ScaleX(455), g_dpi.ScaleY(15), rootWindow, 0, GetModuleHandle(NULL), 0);
		SendMessage(progressBar, PBM_SETRANGE32, 0, 10000);

		HWND static2 = CreateWindowEx(0x20, L"static", L"static2", 0x50000000, g_dpi.ScaleX(15), g_dpi.ScaleY(68), g_dpi.ScaleX(370), g_dpi.ScaleY(25), rootWindow, 0, GetModuleHandle(NULL) /* what?! */, 0);
		SendMessage(static2, WM_SETFONT, (WPARAM)font, 0);

		g_uui.cancelButton = cancelButton;
		g_uui.progressBar = progressBar;
		g_uui.topStatic = static1;
		g_uui.bottomStatic = static2;
	}
	else
	{
		wwidth = 525;
		wheight = 525;

		// make TenUI
		auto ten = std::make_unique<TenUI>();
		ten->uiSource = std::move(DesktopWindowXamlSource{});

		// attach window
		auto interop = ten->uiSource.as<IDesktopWindowXamlSourceNative>();
		winrt::check_hresult(interop->AttachToWindow(rootWindow));

		// setup position
		HWND childHwnd;
		interop->get_WindowHandle(&childHwnd);

		SetWindowLong(childHwnd, GWL_EXSTYLE, GetWindowLong(childHwnd, GWL_EXSTYLE) | WS_EX_TRANSPARENT | WS_EX_LAYERED);

		SetWindowPos(childHwnd, 0, 0, 0, g_dpi.ScaleX(wwidth), g_dpi.ScaleY(wheight), SWP_SHOWWINDOW);

		auto doc = winrt::Windows::UI::Xaml::Markup::XamlReader::Load(g_mainXaml);
		auto ui = doc.as<winrt::Windows::UI::Xaml::FrameworkElement>();

		auto bg = ui.FindName(L"BackdropGrid").as<winrt::Windows::UI::Xaml::Controls::Grid>();
		bg.Background(winrt::make<BackdropBrush>());

		{
			auto sc = ui.FindName(L"Overlay").as<winrt::Windows::UI::Xaml::Controls::SwapChainPanel>();

			if (_time64(NULL) < 1609632000)
			{
				InitializeRenderOverlay(sc, g_dpi.ScaleX(wwidth), g_dpi.ScaleY(wheight));
			}
		}

		/*auto shadow = ui.FindName(L"SharedShadow").as<winrt::Windows::UI::Xaml::Media::ThemeShadow>();
		shadow.Receivers().Append(bg);*/

		ten->topStatic = ui.FindName(L"static1").as<winrt::Windows::UI::Xaml::Controls::TextBlock>();
		ten->bottomStatic = ui.FindName(L"static2").as<winrt::Windows::UI::Xaml::Controls::TextBlock>();
		ten->progressBar = ui.FindName(L"progressBar").as<winrt::Windows::UI::Xaml::Controls::ProgressBar>();

		ten->uiSource.Content(ui);

		g_uui.tenWindow = FindWindowExW(rootWindow, NULL, L"Windows.UI.Core.CoreWindow", NULL);

		g_uui.ten = std::move(ten);
	}

	g_uui.rootWindow = rootWindow;

	RECT wndRect;
	wndRect.left = 0;
	wndRect.top = 0;
	wndRect.right = g_dpi.ScaleX(wwidth);
	wndRect.bottom = g_dpi.ScaleY(wheight);

	HWND desktop = GetDesktopWindow();
	HDC dc = GetDC(desktop);
	int width = GetDeviceCaps(dc, 8);
	int height = GetDeviceCaps(dc, 10);

	ReleaseDC(desktop, dc);

	SetTimer(rootWindow, 0, 20, NULL);

	MoveWindow(rootWindow, (width - g_dpi.ScaleX(wwidth)) / 2, (height - g_dpi.ScaleY(wheight)) / 2, wndRect.right - wndRect.left, wndRect.bottom - wndRect.top, TRUE);

	ShowWindow(rootWindow, TRUE);
}

LRESULT CALLBACK UI_WndProc(HWND hWnd, UINT uMsg, WPARAM wparam, LPARAM lparam)
{
	switch (uMsg)
	{
		case WM_NCHITTEST:
			if (g_uui.tenMode)
			{
				return HTCAPTION;
			}
		case WM_NCCALCSIZE:
			if (g_uui.tenMode)
			{
				return 0;
			}
		case WM_NCCREATE:
			{
				// Only Windows 10+ supports EnableNonClientDpiScaling
				if (IsWindows10OrGreater())
				{
					HMODULE user32 = LoadLibrary(L"user32.dll");

					if (user32)
					{
						auto EnableNonClientDpiScaling = (decltype(&::EnableNonClientDpiScaling))GetProcAddress(user32, "EnableNonClientDpiScaling");

						if (EnableNonClientDpiScaling)
						{
							EnableNonClientDpiScaling(hWnd);
						}

						FreeLibrary(user32);
					}
				}

				return DefWindowProc(hWnd, uMsg, wparam, lparam);
			}
		
		case WM_CTLCOLORSTATIC:
			SetBkMode((HDC)wparam, TRANSPARENT);
			SetTextColor((HDC)wparam, COLORREF(GetSysColor(COLOR_WINDOWTEXT)));

			return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
		case WM_COMMAND:
			if ((HWND)lparam == g_uui.cancelButton)
			{
				g_uui.canceled = true;
			}

			break;
		case WM_TIMER:
			SetWindowText(g_uui.topStatic, g_uui.topText);
			SetWindowText(g_uui.bottomStatic, g_uui.bottomText);
			break;
		case WM_PAINT:
			{
				PAINTSTRUCT ps;
				HDC dc = BeginPaint(hWnd, &ps);
			
				EndPaint(hWnd, &ps);
				break;
			}
		case WM_DPICHANGED:
			{
				// Set the new DPI
				g_dpi.SetScale(LOWORD(wparam), HIWORD(wparam));

				// Resize the window
				LPRECT newScale = (LPRECT)lparam;
				SetWindowPos(hWnd, HWND_TOP, newScale->left, newScale->top, newScale->right - newScale->left, newScale->bottom - newScale->top, SWP_NOZORDER | SWP_NOACTIVATE);

				// Recreate the font
				HFONT newFont = UI_CreateScaledFont(-12, 0, 0, 0, 0, 0, 0, 0, 1, 8, 0, 5, 2, L"Tahoma");

				// Resize all components
				SetWindowPos(g_uui.topStatic, HWND_TOP, g_dpi.ScaleX(15), g_dpi.ScaleY(15), g_dpi.ScaleX(455), g_dpi.ScaleY(25), SWP_SHOWWINDOW);
				SendMessage(g_uui.topStatic, WM_SETFONT, (WPARAM)newFont, 0);

				SetWindowPos(g_uui.cancelButton, HWND_TOP, g_dpi.ScaleX(395), g_dpi.ScaleY(64), g_dpi.ScaleX(75), g_dpi.ScaleY(25), SWP_SHOWWINDOW);
				SendMessage(g_uui.cancelButton, WM_SETFONT, (WPARAM)newFont, 0);

				SetWindowPos(g_uui.progressBar, HWND_TOP, g_dpi.ScaleX(15), g_dpi.ScaleY(40), g_dpi.ScaleX(455), g_dpi.ScaleY(15), SWP_SHOWWINDOW);

				SetWindowPos(g_uui.bottomStatic, HWND_TOP, g_dpi.ScaleX(15), g_dpi.ScaleY(68), g_dpi.ScaleX(370), g_dpi.ScaleY(25), SWP_SHOWWINDOW);
				SendMessage(g_uui.bottomStatic, WM_SETFONT, (WPARAM)newFont, 0);
				break;
			}
		case WM_CLOSE:
			g_uui.canceled = true;
			return 0;
		default:
			if (uMsg == g_uui.taskbarMsg)
			{
				if (g_uui.tbList)
				{
					g_uui.tbList->SetProgressState(hWnd, TBPF_NORMAL);
					g_uui.tbList->SetProgressValue(hWnd, 0, 100);
				}
			}
			break;
	}

	return DefWindowProc(hWnd, uMsg, wparam, lparam);
}

void UI_RegisterClass()
{
	WNDCLASSEX wndClass = { 0 };
	wndClass.cbSize = sizeof(wndClass);
	wndClass.style = 3;
	wndClass.lpfnWndProc = UI_WndProc;
	wndClass.cbClsExtra = 0;
	wndClass.cbWndExtra = 0;
	wndClass.hInstance = GetModuleHandle(NULL);
	wndClass.hIcon = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1));
	wndClass.hCursor = LoadCursor(NULL, (LPCWSTR)0x7F02);
	wndClass.hbrBackground = (HBRUSH)6;
	wndClass.lpszClassName = L"NotSteamAtAll";
	wndClass.hIconSm = LoadIcon(GetModuleHandle(NULL), MAKEINTRESOURCE(1));

	RegisterClassEx(&wndClass);
}

static HANDLE g_actCtx;

bool GenManifest()
{
	if (g_actCtx)
	{
		return true;
	}

	auto hRsc = FindResource(GetModuleHandle(NULL), MAKEINTRESOURCE(1), RT_MANIFEST);

	if (hRsc)
	{
		auto resSize = SizeofResource(GetModuleHandle(NULL), hRsc);
		auto resData = LoadResource(GetModuleHandle(NULL), hRsc);

		auto resPtr = static_cast<const char*>(LockResource(resData));

		wchar_t pathInfo[1024];
		GetModuleFileName(GetModuleHandle(NULL), pathInfo, std::size(pathInfo));

		auto cRef = wcsrchr(pathInfo, L'\\');
		cRef[0] = L'\0';

		auto pathPtr = cRef + 1;

		std::string manifestData{ resPtr, resSize };
		std::wstring wideManifest = ToWide(manifestData);

		boost::algorithm::replace_all(wideManifest, L"DELETE-->", "");
		boost::algorithm::replace_all(wideManifest, L"<!--DELETE", "");
		boost::algorithm::replace_all(wideManifest, L"FiveM_Ship.exe", std::wstring{ pathPtr });

		manifestData = ToNarrow(wideManifest);

		FILE* f = _wfopen(va(L"%s\\FiveM.manifest.xml", pathInfo), L"wb");
		if (f)
		{
			fwrite(manifestData.c_str(), 1, manifestData.size(), f);
			fclose(f);

			ACTCTX ac;
			ac.cbSize = sizeof(ac);
			ac.dwFlags = 0;
			ac.lpSource = va(L"%s\\FiveM.manifest.xml", pathInfo);

			g_actCtx = CreateActCtx(&ac);
			
			if (g_actCtx)
			{
				ULONG_PTR cookie;
				if (ActivateActCtx(g_actCtx, &cookie))
				{
					_wunlink(va(L"%s\\FiveM.manifest.xml", pathInfo));
					return true;
				}
			}
		}
	}

	return false;
}

struct TenUIStorage;

static TenUIStorage* g_tenUI;

struct TenUIStorage : public TenUIBase
{
	WindowsXamlManager manager{ nullptr };

	TenUIStorage()
	{
		g_tenUI = this;
	}

	void InitManager()
	{
		if (!manager)
		{
			manager = WindowsXamlManager::InitializeForCurrentThread();
		}
	}

	virtual ~TenUIStorage() override
	{
		if (manager)
		{
			manager.Close();
		}

		ShowWindow(g_uui.tenWindow, SW_HIDE);

		g_tenUI = nullptr;
	}
};

std::unique_ptr<TenUIBase> UI_InitTen()
{
	bool mf = GenManifest();

	// Windows 10 RS5+ gets a neat UI
	DWORDLONG viMask = 0;
	OSVERSIONINFOEXW osvi = { 0 };
	osvi.dwOSVersionInfoSize = sizeof(osvi);
	osvi.dwBuildNumber = 17763; // RS5+

	VER_SET_CONDITION(viMask, VER_BUILDNUMBER, VER_GREATER_EQUAL);

	bool forceOff = false;

	static HostSharedData<CfxState> initState("CfxInitState");

	if (initState->isReverseGame)
	{
		forceOff = true;
	}

	if (getenv("CitizenFX_NoTenUI"))
	{
		forceOff = true;
	}

#ifdef IS_LAUNCHER
	forceOff = true;
#endif

	if (mf && VerifyVersionInfoW(&osvi, VER_BUILDNUMBER, viMask) && !forceOff)
	{
		RO_REGISTRATION_COOKIE cookie;

		g_uui.tenMode = true;

		try
		{
			return std::make_unique<TenUIStorage>();
		}
		catch (const std::exception&)
		{
		}
	}

	return std::make_unique<TenUIBase>();
}

void UI_DoCreation(bool safeMode)
{
	CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);

	if (g_tenUI)
	{
		g_tenUI->InitManager();
	}

	if (IsWindows7OrGreater())
	{
		CoCreateInstance(CLSID_TaskbarList, 
			NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&g_uui.tbList));
	}

	// Only Windows 8.1+ supports per-monitor DPI awareness
	if (IsWindows8Point1OrGreater())
	{
		HMODULE shCore = LoadLibrary(L"shcore.dll");

		if (shCore)
		{
			auto GetDpiForMonitor = (decltype(&::GetDpiForMonitor))GetProcAddress(shCore, "GetDpiForMonitor");

			if (GetDpiForMonitor)
			{
				UINT dpiX, dpiY;

				POINT point;
				point.x = 1;
				point.y = 1;

				// Get DPI for the main monitor
				HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
				GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dpiX, &dpiY);
				g_dpi.SetScale(dpiX, dpiY);
			}

			FreeLibrary(shCore);
		}
	}

	static bool lastTen = g_uui.tenMode;

	if (safeMode)
	{
		lastTen = g_uui.tenMode;
		g_uui.tenMode = false;
	}
	else
	{
		g_uui.tenMode = lastTen;
	}

	UI_RegisterClass();
	UI_CreateWindow();
}

void UI_DoDestruction()
{
	static HostSharedData<CfxState> initState("CfxInitState");
	AllowSetForegroundWindow((initState->gamePid) ? initState->gamePid : GetCurrentProcessId());

	ShowWindow(g_uui.rootWindow, SW_HIDE);

	if (g_uui.ten)
	{
		if (g_uui.ten->uiSource)
		{
			g_uui.ten->uiSource.Close();
		}
	}

	g_uui.ten = {};

	DestroyWindow(g_uui.rootWindow);
}

void UI_UpdateText(int textControl, const wchar_t* text)
{
	if (g_uui.ten)
	{
		std::wstring tstr = text;

		if (textControl == 0)
		{
			g_uui.ten->topStatic.Text(tstr);
		}
		else
		{
			g_uui.ten->bottomStatic.Text(tstr);
		}

		return;
	}

	if (textControl == 0)
	{
		wcscpy(g_uui.topText, text);
	}
	else
	{
		wcscpy(g_uui.bottomText, text);
	}
}

void UI_UpdateProgress(double percentage)
{
	if (g_uui.ten)
	{
		try
		{
			g_uui.ten->progressBar.Maximum(100.0);
			g_uui.ten->progressBar.Value(percentage);
		}
		catch (...)
		{
		}

		g_uui.ten->progressBar.IsIndeterminate(percentage == 100);

		return;
	}

	SendMessage(g_uui.progressBar, PBM_SETPOS, (int)(percentage * 100), 0);

	if (g_uui.tbList)
	{
		g_uui.tbList->SetProgressValue(g_uui.rootWindow, (int)percentage, 100);

		if (percentage == 100)
		{
			g_uui.tbList->SetProgressState(g_uui.rootWindow, TBPF_NOPROGRESS);
		}
	}
}

bool UI_IsCanceled()
{
	return g_uui.canceled;
}

#include <wrl/module.h>

extern "C" HRESULT __stdcall DllCanUnloadNow()
{
#ifdef _WRL_MODULE_H_
	if (!::Microsoft::WRL::Module<::Microsoft::WRL::InProc>::GetModule().Terminate())
	{
		return 1; // S_FALSE
	}
#endif

	if (winrt::get_module_lock())
	{
		return 1; // S_FALSE
	}

	winrt::clear_factory_cache();
	return 0; // S_OK
}

extern "C" DLL_EXPORT HRESULT WINRT_CALL DllGetActivationFactory(HSTRING classId, IActivationFactory** factory)
{
	try
	{
		*factory = nullptr;
		uint32_t length{};
		wchar_t const* const buffer = WINRT_WindowsGetStringRawBuffer(classId, &length);
		std::wstring_view const name{ buffer, length };

		auto requal = [](std::wstring_view const& left, std::wstring_view const& right) noexcept
		{
			return std::equal(left.rbegin(), left.rend(), right.rbegin(), right.rend());
		};

		if (requal(name, L"CitiLaunch.BackdropBrush"))
		{
			*factory = (IActivationFactory*)winrt::detach_abi(winrt::make<BackdropBrush>());
			return 0;
		}

#ifdef _WRL_MODULE_H_
		return ::Microsoft::WRL::Module<::Microsoft::WRL::InProc>::GetModule().GetActivationFactory(static_cast<HSTRING>(classId), reinterpret_cast<::IActivationFactory * *>(factory));
#else
		return winrt::hresult_class_not_available(name).to_abi();
#endif
	}
	catch (...) { return winrt::to_hresult(); }
}

WrlCreatorMapIncludePragma(ColorSourceEffect);
WrlCreatorMapIncludePragma(ColorMatrixEffect);
WrlCreatorMapIncludePragma(Transform2DEffect);
WrlCreatorMapIncludePragma(CompositeEffect);
#else
void UI_DoCreation(bool safeMode)
{
}
void UI_DoDestruction()
{
}
void UI_UpdateText(int textControl, const wchar_t* text)
{
}
void UI_UpdateProgress(double percentage)
{
}
bool UI_IsCanceled()
{
	return true;
}
HWND UI_GetWindowHandle()
{
	return NULL;
}
std::unique_ptr<TenUIBase> UI_InitTen()
{
	return {};
}

DLL_EXPORT HRESULT DllCanUnloadNow()
{
	return S_OK;
}

#pragma comment(lib, "delayimp.lib")
#endif
