/*
-----------------------------------------------------------------------------
This source file is part of OGRE
(Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-2011 Torus Knot Software Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/
#include "CmD3D9Device.h"
#include "CmD3D9DeviceManager.h"
#include "CmD3D9Driver.h"
#include "CmD3D9RenderSystem.h"
#include "CmD3D9ResourceManager.h"
#include "CmD3D9RenderWindow.h"
#include "CmHardwareBufferManager.h"
#include "CmException.h"
#include "CmRenderSystemManager.h"

namespace CamelotEngine
{
	HWND D3D9Device::msSharedFocusWindow = NULL;

	//---------------------------------------------------------------------
	D3D9Device::D3D9Device(D3D9DeviceManager* deviceManager,
		UINT adapterNumber, 
		HMONITOR hMonitor, 
		D3DDEVTYPE devType, 
		DWORD behaviorFlags)
	{
		mpDeviceManager				= deviceManager;
		mpDevice					= NULL;		
		mAdapterNumber				= adapterNumber;
		mMonitor					= hMonitor;
		mDeviceType					= devType;
		mFocusWindow				= NULL;
		mBehaviorFlags				= behaviorFlags;	
		mD3D9DeviceCapsValid		= false;
		mDeviceLost					= false;
		mPresentationParamsCount 	= 0;
		mPresentationParams		 	= NULL;
		memset(&mD3D9DeviceCaps, 0, sizeof(mD3D9DeviceCaps));
		memset(&mCreationParams, 0, sizeof(mCreationParams));		
	}

	//---------------------------------------------------------------------
	D3D9Device::~D3D9Device()
	{

	}

	//---------------------------------------------------------------------
	D3D9Device::RenderWindowToResorucesIterator D3D9Device::getRenderWindowIterator(D3D9RenderWindow* renderWindow)
	{
		RenderWindowToResorucesIterator it = mMapRenderWindowToResoruces.find(renderWindow);

		if (it == mMapRenderWindowToResoruces.end())
		{
			OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
				"Render window was not attached to this device !!", 
				"D3D9Device::getRenderWindowIterator" );
		}

		return it;
	}


	//---------------------------------------------------------------------
	void D3D9Device::attachRenderWindow(D3D9RenderWindow* renderWindow)
	{
		RenderWindowToResorucesIterator it = mMapRenderWindowToResoruces.find(renderWindow);

		if (it == mMapRenderWindowToResoruces.end())
		{
			RenderWindowResources* renderWindowResources = new RenderWindowResources;

			memset(renderWindowResources, 0, sizeof(RenderWindowResources));						
			renderWindowResources->adapterOrdinalInGroupIndex = 0;					
			renderWindowResources->acquired = false;
			mMapRenderWindowToResoruces[renderWindow] = renderWindowResources;			
		}
		updateRenderWindowsIndices();
	}

	//---------------------------------------------------------------------
	void D3D9Device::detachRenderWindow(D3D9RenderWindow* renderWindow)
	{
		RenderWindowToResorucesIterator it = mMapRenderWindowToResoruces.find(renderWindow);

		if (it != mMapRenderWindowToResoruces.end())
		{		
			// The focus window in which the d3d9 device created on is detached.
			// resources must be acquired again.
			if (mFocusWindow == renderWindow->getWindowHandle())
			{
				mFocusWindow = NULL;				
			}

			// Case this is the shared focus window.
			if (renderWindow->getWindowHandle() == msSharedFocusWindow)			
				setSharedWindowHandle(NULL);		
			
			RenderWindowResources* renderWindowResources = it->second;

			releaseRenderWindowResources(renderWindowResources);

			SAFE_DELETE(renderWindowResources);
			
			mMapRenderWindowToResoruces.erase(it);		
		}
		updateRenderWindowsIndices();
	}

	//---------------------------------------------------------------------
	bool D3D9Device::acquire()
	{	
		updatePresentationParameters();

		bool resetDevice = false;
			
		// Create device if need to.
		if (mpDevice == NULL)
		{			
			createD3D9Device();
		}

		// Case device already exists.
		else
		{
			RenderWindowToResorucesIterator itPrimary = getRenderWindowIterator(getPrimaryWindow());

			if (itPrimary->second->swapChain != NULL)
			{
				D3DPRESENT_PARAMETERS currentPresentParams;
				HRESULT hr;

				hr = itPrimary->second->swapChain->GetPresentParameters(&currentPresentParams);
				if (FAILED(hr))
				{
					OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
						"GetPresentParameters failed", 
						"D3D9RenderWindow::acquire");
				}
				
				// Desired parameters are different then current parameters.
				// Possible scenario is that primary window resized and in the mean while another
				// window attached to this device.
				if (memcmp(&currentPresentParams, &mPresentationParams[0], sizeof(D3DPRESENT_PARAMETERS)) != 0)
				{
					resetDevice = true;					
				}				
			}

			// Make sure depth stencil is set to valid surface. It is going to be
			// grabbed by the primary window using the GetDepthStencilSurface method.
			if (resetDevice == false)
			{
				mpDevice->SetDepthStencilSurface(itPrimary->second->depthBuffer);
			}
			
		}

		// Reset device will update all render window resources.
		if (resetDevice)
		{
			reset();
		}

		// No need to reset -> just acquire resources.
		else
		{
			// Update resources of each window.
			RenderWindowToResorucesIterator it = mMapRenderWindowToResoruces.begin();

			while (it != mMapRenderWindowToResoruces.end())
			{
				acquireRenderWindowResources(it);
				++it;
			}
		}
									
		return true;
	}

	//---------------------------------------------------------------------
	void D3D9Device::release()
	{
		if (mpDevice != NULL)
		{
			D3D9RenderSystem* renderSystem = static_cast<D3D9RenderSystem*>(CamelotEngine::RenderSystemManager::getActive());

			//// Clean up depth stencil surfaces
			//renderSystem->_cleanupDepthStencils(mpDevice);	

			RenderWindowToResorucesIterator it = mMapRenderWindowToResoruces.begin();

			while (it != mMapRenderWindowToResoruces.end())
			{
				RenderWindowResources* renderWindowResources = it->second;

				releaseRenderWindowResources(renderWindowResources);
				++it;
			}

			releaseD3D9Device();
		}				
	}

	//---------------------------------------------------------------------
	bool D3D9Device::acquire(D3D9RenderWindow* renderWindow)
	{
		RenderWindowToResorucesIterator it = getRenderWindowIterator(renderWindow);
		
		acquireRenderWindowResources(it);

		return true;
	}

	//---------------------------------------------------------------------	
	void D3D9Device::notifyDeviceLost()
	{
		// Case this device is already in lost state.
		if (mDeviceLost)
			return;

		// Case we just moved from valid state to lost state.
		mDeviceLost = true;	

		D3D9RenderSystem* renderSystem = static_cast<D3D9RenderSystem*>(CamelotEngine::RenderSystemManager::getActive());

		renderSystem->notifyOnDeviceLost(this);
	}	

	//---------------------------------------------------------------------
	IDirect3DSurface9* D3D9Device::getDepthBuffer(D3D9RenderWindow* renderWindow)
	{
		RenderWindowToResorucesIterator it = getRenderWindowIterator(renderWindow);		

		return it->second->depthBuffer;
	}

	//---------------------------------------------------------------------
	IDirect3DSurface9* D3D9Device::getBackBuffer(D3D9RenderWindow* renderWindow)
	{
		RenderWindowToResorucesIterator it = getRenderWindowIterator(renderWindow);
	
		return it->second->backBuffer;		
	}

	//---------------------------------------------------------------------
	UINT32 D3D9Device::getRenderWindowCount() const
	{
		return static_cast<UINT32>(mMapRenderWindowToResoruces.size());
	}

	//---------------------------------------------------------------------
	D3D9RenderWindow* D3D9Device::getRenderWindow(UINT32 index)
	{
		if (index >= mMapRenderWindowToResoruces.size())
		{
			OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
				"Index of render window is out of bounds!", 
				"D3D9RenderWindow::getRenderWindow");
		}
		
		RenderWindowToResorucesIterator it = mMapRenderWindowToResoruces.begin();

		while (it != mMapRenderWindowToResoruces.end())
		{
			if (index == 0)			
				break;			
			
			--index;
			++it;
		}
		
		return it->first;
	}

	//---------------------------------------------------------------------
	void D3D9Device::setAdapterOrdinalIndex(D3D9RenderWindow* renderWindow, UINT32 adapterOrdinalInGroupIndex)
	{
		RenderWindowToResorucesIterator it = getRenderWindowIterator(renderWindow);

		it->second->adapterOrdinalInGroupIndex = adapterOrdinalInGroupIndex;

		updateRenderWindowsIndices();
	}
	
	//---------------------------------------------------------------------
	void D3D9Device::destroy()
	{	
		// Lock access to rendering device.
		D3D9RenderSystem::getResourceManager()->lockDeviceAccess();
		
		release();
		
		RenderWindowToResorucesIterator it = mMapRenderWindowToResoruces.begin();

		if (it != mMapRenderWindowToResoruces.end())
		{	
			if (it->first->getWindowHandle() == msSharedFocusWindow)
				setSharedWindowHandle(NULL);

			SAFE_DELETE(it->second);
			++it;
		}
		mMapRenderWindowToResoruces.clear();
		
		// Reset dynamic attributes.		
		mFocusWindow			= NULL;		
		mD3D9DeviceCapsValid	= false;
		SAFE_DELETE_ARRAY(mPresentationParams);
		mPresentationParamsCount = 0;

		// Notify the device manager on this instance destruction.	
		mpDeviceManager->notifyOnDeviceDestroy(this);

		// UnLock access to rendering device.
		D3D9RenderSystem::getResourceManager()->unlockDeviceAccess();
	}	
	
	//---------------------------------------------------------------------
	bool D3D9Device::isDeviceLost()
	{		
		HRESULT hr;

		hr = mpDevice->TestCooperativeLevel();

		if (hr == D3DERR_DEVICELOST ||
			hr == D3DERR_DEVICENOTRESET)
		{
			return true;
		}
		
		return false;
	}

	//---------------------------------------------------------------------
	bool D3D9Device::reset()
	{
		HRESULT hr;

		// Check that device is in valid state for reset.
		hr = mpDevice->TestCooperativeLevel();
		if (hr == D3DERR_DEVICELOST ||
			hr == D3DERR_DRIVERINTERNALERROR)
		{
			return false;
		}

		// Lock access to rendering device.
		D3D9RenderSystem::getResourceManager()->lockDeviceAccess();
					
		D3D9RenderSystem* renderSystem = static_cast<D3D9RenderSystem*>(CamelotEngine::RenderSystemManager::getActive());

		// Inform all resources that device lost.
		D3D9RenderSystem::getResourceManager()->notifyOnDeviceLost(mpDevice);

		// Notify all listener before device is rested
		renderSystem->notifyOnDeviceLost(this);

		// Cleanup depth stencils surfaces.
		renderSystem->_cleanupDepthStencils(mpDevice);

		updatePresentationParameters();

		RenderWindowToResorucesIterator it = mMapRenderWindowToResoruces.begin();

		while (it != mMapRenderWindowToResoruces.end())
		{
			RenderWindowResources* renderWindowResources = it->second;

			releaseRenderWindowResources(renderWindowResources);
			++it;
		}

		clearDeviceStreams();


		// Reset the device using the presentation parameters.
		hr = mpDevice->Reset(mPresentationParams);
	
		if (hr == D3DERR_DEVICELOST)
		{
			// UnLock access to rendering device.
			D3D9RenderSystem::getResourceManager()->unlockDeviceAccess();

			// Don't continue
			return false;
		}
		else if (FAILED(hr))
		{
			OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
				"Cannot reset device!", 
				"D3D9RenderWindow::reset");
		}

		mDeviceLost = false;

		// Initialize device states.
		setupDeviceStates();

		// Update resources of each window.
		it = mMapRenderWindowToResoruces.begin();

		while (it != mMapRenderWindowToResoruces.end())
		{
			acquireRenderWindowResources(it);
			++it;
		}		

		D3D9Device* pCurActiveDevice = mpDeviceManager->getActiveDevice();

		mpDeviceManager->setActiveDevice(this);

		// Inform all resources that device has been reset.
		D3D9RenderSystem::getResourceManager()->notifyOnDeviceReset(mpDevice);

		mpDeviceManager->setActiveDevice(pCurActiveDevice);
		
		renderSystem->notifyOnDeviceReset(this);

		// UnLock access to rendering device.
		D3D9RenderSystem::getResourceManager()->unlockDeviceAccess();
	
		return true;
	}

	//---------------------------------------------------------------------
	bool D3D9Device::isAutoDepthStencil() const
	{
		const D3DPRESENT_PARAMETERS& primaryPresentationParams = mPresentationParams[0];
		
		// Check if auto depth stencil can be used.
		for (unsigned int i = 1; i < mPresentationParamsCount; i++)
		{			
			// disable AutoDepthStencil if these parameters are not all the same.
			if(primaryPresentationParams.BackBufferHeight != mPresentationParams[i].BackBufferHeight || 
				primaryPresentationParams.BackBufferWidth != mPresentationParams[i].BackBufferWidth	|| 
				primaryPresentationParams.BackBufferFormat != mPresentationParams[i].BackBufferFormat || 
				primaryPresentationParams.AutoDepthStencilFormat != mPresentationParams[i].AutoDepthStencilFormat ||
				primaryPresentationParams.MultiSampleQuality != mPresentationParams[i].MultiSampleQuality ||
				primaryPresentationParams.MultiSampleType != mPresentationParams[i].MultiSampleType)
			{
				return false;
			}
		}

		return true;
	}

	//---------------------------------------------------------------------
	const D3DCAPS9& D3D9Device::getD3D9DeviceCaps() const
	{
		if (mD3D9DeviceCapsValid == false)
		{
			OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
				"Device caps are invalid!", 
				"D3D9Device::getD3D9DeviceCaps" );
		}

		return mD3D9DeviceCaps;
	}

	//---------------------------------------------------------------------
	D3DFORMAT D3D9Device::getBackBufferFormat() const
	{		
		if (mPresentationParams == NULL || mPresentationParamsCount == 0)
		{
			OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
				"Presentation parameters are invalid!", 
				"D3D9Device::getBackBufferFormat" );
		}

		return mPresentationParams[0].BackBufferFormat;
	}

	//---------------------------------------------------------------------
	IDirect3DDevice9* D3D9Device::getD3D9Device()
	{
		return mpDevice;
	}

	//---------------------------------------------------------------------
	void D3D9Device::updatePresentationParameters()
	{		
		// Clear old presentation parameters.
		SAFE_DELETE_ARRAY(mPresentationParams);
		mPresentationParamsCount = 0;		

		if (mMapRenderWindowToResoruces.size() > 0)
		{
			mPresentationParams = new D3DPRESENT_PARAMETERS[mMapRenderWindowToResoruces.size()];

			RenderWindowToResorucesIterator it = mMapRenderWindowToResoruces.begin();

			while (it != mMapRenderWindowToResoruces.end())
			{
				D3D9RenderWindow* renderWindow = it->first;
				RenderWindowResources* renderWindowResources = it->second;

				// Ask the render window to build it's own parameters.
				renderWindow->buildPresentParameters(&renderWindowResources->presentParameters);
				

				// Update shared focus window handle.
				if (renderWindow->isFullScreen() && 
					renderWindowResources->presentParametersIndex == 0 &&
					msSharedFocusWindow == NULL)
					setSharedWindowHandle(renderWindow->getWindowHandle());					

				// This is the primary window or a full screen window that is part of multi head device.
				if (renderWindowResources->presentParametersIndex == 0 ||
					renderWindow->isFullScreen())
				{
					mPresentationParams[renderWindowResources->presentParametersIndex] = renderWindowResources->presentParameters;
					mPresentationParamsCount++;
				}
															
				++it;
			}
		}

		// Case we have to cancel auto depth stencil.
		if (isMultihead() && isAutoDepthStencil() == false)
		{
			for(unsigned int i = 0; i < mPresentationParamsCount; i++)
			{
				mPresentationParams[i].EnableAutoDepthStencil = false;
			}
		}
	}	

	//---------------------------------------------------------------------
	UINT D3D9Device::getAdapterNumber() const
	{
		return mAdapterNumber;
	}

	//---------------------------------------------------------------------
	D3DDEVTYPE D3D9Device::getDeviceType() const
	{
		return mDeviceType;
	}

	//---------------------------------------------------------------------
	bool D3D9Device::isMultihead() const
	{
		RenderWindowToResorucesMap::const_iterator it = mMapRenderWindowToResoruces.begin();

		while (it != mMapRenderWindowToResoruces.end())				
		{
			RenderWindowResources* renderWindowResources = it->second;
			
			if (renderWindowResources->adapterOrdinalInGroupIndex > 0 &&
				it->first->isFullScreen())
			{
				return true;
			}
			
			++it;		
		}

		return false;
	}

	//---------------------------------------------------------------------
	void D3D9Device::clearDeviceStreams()
	{
		D3D9RenderSystem* renderSystem = static_cast<D3D9RenderSystem*>(CamelotEngine::RenderSystemManager::getActive());

		// Set all texture units to nothing to release texture surfaces
		for (DWORD stage = 0; stage < mD3D9DeviceCaps.MaxSimultaneousTextures; ++stage)
		{
			DWORD   dwCurValue = D3DTOP_FORCE_DWORD;
			HRESULT hr;

			hr = mpDevice->SetTexture(stage, NULL);
			if( hr != S_OK )
			{
				String str = "Unable to disable texture '" + StringConverter::toString((unsigned int)stage) + "' in D3D9";
				OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, str, "D3D9Device::clearDeviceStreams" );
			}
		
			mpDevice->GetTextureStageState(static_cast<DWORD>(stage), D3DTSS_COLOROP, &dwCurValue);

			if (dwCurValue != D3DTOP_DISABLE)
			{
				hr = mpDevice->SetTextureStageState(static_cast<DWORD>(stage), D3DTSS_COLOROP, D3DTOP_DISABLE);
				if( hr != S_OK )
				{
					String str = "Unable to disable texture '" + StringConverter::toString((unsigned)stage) + "' in D3D9";
					OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, str, "D3D9Device::clearDeviceStreams" );
				}
			}			
		
			// set stage desc. to defaults
			renderSystem->mTexStageDesc[stage].pTex = 0;
			renderSystem->mTexStageDesc[stage].autoTexCoordType = TEXCALC_NONE;
			renderSystem->mTexStageDesc[stage].coordIndex = 0;
			renderSystem->mTexStageDesc[stage].texType = D3D9Mappings::D3D_TEX_TYPE_NORMAL;
		}

		// Unbind any vertex streams to avoid memory leaks				
		for (unsigned int i = 0; i < mD3D9DeviceCaps.MaxStreams; ++i)
		{
			mpDevice->SetStreamSource(i, NULL, 0, 0);
		}
	}

	//---------------------------------------------------------------------
	void D3D9Device::createD3D9Device()
	{		
		// Update focus window.
		D3D9RenderWindow* primaryRenderWindow = getPrimaryWindow();

		// Case we have to share the same focus window.
		if (msSharedFocusWindow != NULL)
			mFocusWindow = msSharedFocusWindow;
		else
			mFocusWindow = primaryRenderWindow->getWindowHandle();		

		IDirect3D9* pD3D9 = D3D9RenderSystem::getDirect3D9();
		HRESULT     hr;


		if (isMultihead())
		{
			mBehaviorFlags |= D3DCREATE_ADAPTERGROUP_DEVICE;
		}		
		else
		{
			mBehaviorFlags &= ~D3DCREATE_ADAPTERGROUP_DEVICE;
		}

		// Try to create the device with hardware vertex processing. 
		mBehaviorFlags |= D3DCREATE_HARDWARE_VERTEXPROCESSING;
		hr = pD3D9->CreateDevice(mAdapterNumber, mDeviceType, mFocusWindow,
			mBehaviorFlags, mPresentationParams, &mpDevice);

		if (FAILED(hr))
		{
			// Try a second time, may fail the first time due to back buffer count,
			// which will be corrected down to 1 by the runtime
			hr = pD3D9->CreateDevice(mAdapterNumber, mDeviceType, mFocusWindow,
				mBehaviorFlags, mPresentationParams, &mpDevice);
		}

		// Case hardware vertex processing failed.
		if( FAILED( hr ) )
		{
			// Try to create the device with mixed vertex processing.
			mBehaviorFlags &= ~D3DCREATE_HARDWARE_VERTEXPROCESSING;
			mBehaviorFlags |= D3DCREATE_MIXED_VERTEXPROCESSING;

			hr = pD3D9->CreateDevice(mAdapterNumber, mDeviceType, mFocusWindow,
				mBehaviorFlags, mPresentationParams, &mpDevice);
		}

		if( FAILED( hr ) )
		{
			// try to create the device with software vertex processing.
			mBehaviorFlags &= ~D3DCREATE_MIXED_VERTEXPROCESSING;
			mBehaviorFlags |= D3DCREATE_SOFTWARE_VERTEXPROCESSING;
			hr = pD3D9->CreateDevice(mAdapterNumber, mDeviceType, mFocusWindow,
				mBehaviorFlags, mPresentationParams, &mpDevice);
		}

		if ( FAILED( hr ) )
		{
			// try reference device
			mDeviceType = D3DDEVTYPE_REF;
			hr = pD3D9->CreateDevice(mAdapterNumber, mDeviceType, mFocusWindow,
				mBehaviorFlags, mPresentationParams, &mpDevice);

			if ( FAILED( hr ) )
			{
				OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
					"Cannot create device!", 
					"D3D9Device::createD3D9Device" );
			}
		}

		// Get current device caps.
		hr = mpDevice->GetDeviceCaps(&mD3D9DeviceCaps);
		if( FAILED( hr ) )
		{
			OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
				"Cannot get device caps!", 
				"D3D9Device::createD3D9Device" );
		}

		// Get current creation parameters caps.
		hr = mpDevice->GetCreationParameters(&mCreationParams);
		if ( FAILED(hr) )
		{
			OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
				"Error Get Creation Parameters", 
				"D3D9Device:createD3D9Device" );
		}

		mD3D9DeviceCapsValid = true;
			
		// Initialize device states.
		setupDeviceStates();

		// Lock access to rendering device.
		D3D9RenderSystem::getResourceManager()->lockDeviceAccess();

		D3D9Device* pCurActiveDevice = mpDeviceManager->getActiveDevice();

		mpDeviceManager->setActiveDevice(this);

		// Inform all resources that new device created.
		D3D9RenderSystem::getResourceManager()->notifyOnDeviceCreate(mpDevice);

		mpDeviceManager->setActiveDevice(pCurActiveDevice);

		// UnLock access to rendering device.
		D3D9RenderSystem::getResourceManager()->unlockDeviceAccess();
	}

	//---------------------------------------------------------------------
	void D3D9Device::releaseD3D9Device()
	{
		if (mpDevice != NULL)
		{
			// Lock access to rendering device.
			D3D9RenderSystem::getResourceManager()->lockDeviceAccess();

			D3D9Device* pCurActiveDevice = mpDeviceManager->getActiveDevice();

			mpDeviceManager->setActiveDevice(this);

			// Inform all resources that device is going to be destroyed.
			D3D9RenderSystem::getResourceManager()->notifyOnDeviceDestroy(mpDevice);

			mpDeviceManager->setActiveDevice(pCurActiveDevice);
			
			clearDeviceStreams();		

			// Release device.
			SAFE_RELEASE(mpDevice);	
			
			// UnLock access to rendering device.
			D3D9RenderSystem::getResourceManager()->unlockDeviceAccess();
		}
	}

	//---------------------------------------------------------------------
	void D3D9Device::releaseRenderWindowResources(RenderWindowResources* renderWindowResources)
	{
		SAFE_RELEASE(renderWindowResources->backBuffer);
		SAFE_RELEASE(renderWindowResources->depthBuffer);
		SAFE_RELEASE(renderWindowResources->swapChain);
		renderWindowResources->acquired = false;
	}

	//---------------------------------------------------------------------
	void D3D9Device::invalidate(D3D9RenderWindow* renderWindow)
	{
		RenderWindowToResorucesIterator it = getRenderWindowIterator(renderWindow);

		it->second->acquired = false;		
	}

	//---------------------------------------------------------------------
	bool D3D9Device::validate(D3D9RenderWindow* renderWindow)
	{
		// Validate that the render window should run on this device.
		if (false == validateDisplayMonitor(renderWindow))
			return false;
		
		// Validate that this device created on the correct target focus window handle		
		validateFocusWindow();		
		
		// Validate that the render window dimensions matches to back buffer dimensions.
		validateBackBufferSize(renderWindow);

		// Validate that this device is in valid rendering state.
		if (false == validateDeviceState(renderWindow))
			return false;

		return true;
	}

	//---------------------------------------------------------------------
	void D3D9Device::validateFocusWindow()
	{
		// Focus window changed -> device should be re-acquired.
		if ((msSharedFocusWindow != NULL && mCreationParams.hFocusWindow != msSharedFocusWindow) ||
			(msSharedFocusWindow == NULL && mCreationParams.hFocusWindow != getPrimaryWindow()->getWindowHandle()))
		{
			// Lock access to rendering device.
			D3D9RenderSystem::getResourceManager()->lockDeviceAccess();

			release();
			acquire();

			// UnLock access to rendering device.
			D3D9RenderSystem::getResourceManager()->unlockDeviceAccess();
		}
	}

	//---------------------------------------------------------------------
	bool D3D9Device::validateDeviceState(D3D9RenderWindow* renderWindow)
	{
		RenderWindowToResorucesIterator it = getRenderWindowIterator(renderWindow);		
		RenderWindowResources* renderWindowResources =  it->second;
		HRESULT hr;

		hr = mpDevice->TestCooperativeLevel();	

		// Case device is not valid for rendering. 
		if (FAILED(hr))
		{					
			// device lost, and we can't reset
			// can't do anything about it here, wait until we get 
			// D3DERR_DEVICENOTRESET; rendering calls will silently fail until 
			// then (except Present, but we ignore device lost there too)
			if (hr == D3DERR_DEVICELOST)
			{						
				releaseRenderWindowResources(renderWindowResources);
				notifyDeviceLost();							
				return false;
			}

			// device lost, and we can reset
			else if (hr == D3DERR_DEVICENOTRESET)
			{					
				bool deviceRestored = reset();

				// Device was not restored yet.
				if (deviceRestored == false)
				{
					// Wait a while
					Sleep(50);					
					return false;
				}																								
			}						
		}

		// Render window resources explicitly invalidated. (Resize or window mode switch) 
		if (renderWindowResources->acquired == false)
		{
			if (getPrimaryWindow() == renderWindow)
			{
				bool deviceRestored = reset();

				// Device was not restored yet.
				if (deviceRestored == false)
				{
					// Wait a while
					Sleep(50);					
					return false;
				}	
			}
			else
			{
				acquire(renderWindow);
			}
		}

		return true;
	}
		

	//---------------------------------------------------------------------
	void D3D9Device::validateBackBufferSize(D3D9RenderWindow* renderWindow)
	{
		RenderWindowToResorucesIterator it = getRenderWindowIterator(renderWindow);
		RenderWindowResources*	renderWindowResources = it->second;
	

		// Case size has been changed.
		if (renderWindow->getWidth() != renderWindowResources->presentParameters.BackBufferWidth ||
			renderWindow->getHeight() != renderWindowResources->presentParameters.BackBufferHeight)
		{			
			if (renderWindow->getWidth() > 0)
				renderWindowResources->presentParameters.BackBufferWidth = renderWindow->getWidth();

			if (renderWindow->getHeight() > 0)
				renderWindowResources->presentParameters.BackBufferHeight = renderWindow->getHeight();

			invalidate(renderWindow);
		}				
	}

	//---------------------------------------------------------------------
	bool D3D9Device::validateDisplayMonitor(D3D9RenderWindow* renderWindow)
	{
		// Ignore full screen since it doesn't really move and it is possible 
		// that it created using multi-head adapter so for a subordinate the
		// native monitor handle and this device handle will be different.
		if (renderWindow->isFullScreen())
			return true;

		RenderWindowToResorucesIterator it = getRenderWindowIterator(renderWindow);
		HMONITOR	hRenderWindowMonitor = NULL;

		// Find the monitor this render window belongs to.
		hRenderWindowMonitor = MonitorFromWindow(renderWindow->getWindowHandle(), MONITOR_DEFAULTTONULL);

		// This window doesn't intersect with any of the display monitor
		if (hRenderWindowMonitor == NULL)		
			return false;		
		

		// Case this window changed monitor.
		if (hRenderWindowMonitor != mMonitor)
		{	
			// Lock access to rendering device.
			D3D9RenderSystem::getResourceManager()->lockDeviceAccess();

			mpDeviceManager->linkRenderWindow(renderWindow);

			// UnLock access to rendering device.
			D3D9RenderSystem::getResourceManager()->unlockDeviceAccess();

			return false;
		}

		return true;
	}

	//---------------------------------------------------------------------
	void D3D9Device::present(D3D9RenderWindow* renderWindow)
	{		
		RenderWindowToResorucesIterator it = getRenderWindowIterator(renderWindow);
		RenderWindowResources*	renderWindowResources = it->second;				


		// Skip present while current device state is invalid.
		if (mDeviceLost || 
			renderWindowResources->acquired == false || 
			isDeviceLost())		
			return;		


		HRESULT hr;

		if (isMultihead())
		{
			// Only the master will call present method results in synchronized
			// buffer swap for the rest of the implicit swap chain.
			if (getPrimaryWindow() == renderWindow)
				hr = mpDevice->Present( NULL, NULL, NULL, NULL );
			else
				hr = S_OK;
		}
		else
		{
			hr = renderWindowResources->swapChain->Present(NULL, NULL, NULL, NULL, 0);			
		}


		if( D3DERR_DEVICELOST == hr)
		{
			releaseRenderWindowResources(renderWindowResources);
			notifyDeviceLost();
		}
		else if( FAILED(hr) )
		{
			OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
				"Error Presenting surfaces", 
				"D3D9Device::present" );
		}
		else
		{
			// TODO PORT - What is this used for? In any case I don't have Root so just set it to 0
			//mLastPresentFrame = Root::getSingleton().getNextFrameNumber();
			mLastPresentFrame = 0;
		}
	}

	//---------------------------------------------------------------------
	void D3D9Device::acquireRenderWindowResources(RenderWindowToResorucesIterator it)
	{
		RenderWindowResources*	renderWindowResources = it->second;
		D3D9RenderWindow*		renderWindow = it->first;			
		
		releaseRenderWindowResources(renderWindowResources);

		// Create additional swap chain
		if (isSwapChainWindow(renderWindow) && !isMultihead())
		{
			// Create swap chain
			HRESULT hr = mpDevice->CreateAdditionalSwapChain(&renderWindowResources->presentParameters, 
				&renderWindowResources->swapChain);

			if (FAILED(hr))
			{
				// Try a second time, may fail the first time due to back buffer count,
				// which will be corrected by the runtime
				hr = mpDevice->CreateAdditionalSwapChain(&renderWindowResources->presentParameters, 
					&renderWindowResources->swapChain);
			}

			if (FAILED(hr))
			{
				OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
					"Unable to create an additional swap chain",
					"D3D9RenderWindow::acquireRenderWindowResources");
			}
		}
		else
		{
			// The swap chain is already created by the device
			HRESULT hr = mpDevice->GetSwapChain(renderWindowResources->presentParametersIndex, 
				&renderWindowResources->swapChain);
			if (FAILED(hr)) 
			{
				OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
					"Unable to get the swap chain",
					"D3D9RenderWindow::acquireRenderWindowResources");
			}
		}

		// Store references to buffers for convenience		
		renderWindowResources->swapChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, 
			&renderWindowResources->backBuffer);

		// Additional swap chains need their own depth buffer
		// to support resizing them
		if (renderWindow->isDepthBuffered()) 
		{
			// if multihead is enabled, depth buffer can be created automatically for 
			// all the adapters. if multihead is not enabled, depth buffer is just
			// created for the main swap chain
			if (isMultihead() && isAutoDepthStencil() || 
			    isMultihead() == false && isSwapChainWindow(renderWindow) == false)
			{
				mpDevice->GetDepthStencilSurface(&renderWindowResources->depthBuffer);
			}
			else
			{
				UINT32 targetWidth  = renderWindow->getWidth();
				UINT32 targetHeight = renderWindow->getHeight();

				if (targetWidth == 0)
					targetWidth = 1;

				if (targetHeight == 0)
					targetHeight = 1;

				HRESULT hr = mpDevice->CreateDepthStencilSurface(
					targetWidth, targetHeight,
					renderWindowResources->presentParameters.AutoDepthStencilFormat,
					renderWindowResources->presentParameters.MultiSampleType,
					renderWindowResources->presentParameters.MultiSampleQuality, 
					(renderWindowResources->presentParameters.Flags & D3DPRESENTFLAG_DISCARD_DEPTHSTENCIL),
					&renderWindowResources->depthBuffer, NULL
					);

				if (FAILED(hr)) 
				{
					OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
						"Unable to create a depth buffer for the swap chain",
						"D3D9RenderWindow::acquireRenderWindowResources");
				}

				if (isSwapChainWindow(renderWindow) == false)
				{
					mpDevice->SetDepthStencilSurface(renderWindowResources->depthBuffer);
				}
			}
		}

		renderWindowResources->acquired = true; 
	}

	//---------------------------------------------------------------------
	void D3D9Device::setupDeviceStates()
	{
		HRESULT hr = mpDevice->SetRenderState(D3DRS_SPECULARENABLE, TRUE);
		
		if (FAILED(hr)) 
		{
			OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR, 
				"Unable to apply render state: D3DRS_SPECULARENABLE <- TRUE",
				"D3D9Device::setupDeviceStates");
		}		
	}

	//---------------------------------------------------------------------
	bool D3D9Device::isSwapChainWindow(D3D9RenderWindow* renderWindow)
	{
		RenderWindowToResorucesIterator it = getRenderWindowIterator(renderWindow);
		
		if (it->second->presentParametersIndex == 0 || renderWindow->isFullScreen())			
			return false;
			
		return true;
	}

	//---------------------------------------------------------------------
	D3D9RenderWindow* D3D9Device::getPrimaryWindow()
	{		
		RenderWindowToResorucesIterator it = mMapRenderWindowToResoruces.begin();
	
		while (it != mMapRenderWindowToResoruces.end() && it->second->presentParametersIndex != 0)				
			++it;		

		assert(it != mMapRenderWindowToResoruces.end());

		return it->first;
	}

	//---------------------------------------------------------------------
	void D3D9Device::setSharedWindowHandle(HWND hSharedHWND)
	{
		if (hSharedHWND != msSharedFocusWindow)					
			msSharedFocusWindow = hSharedHWND;					
	}

	//---------------------------------------------------------------------
	void D3D9Device::updateRenderWindowsIndices()
	{
		// Update present parameters index attribute per render window.
		if (isMultihead())
		{
			RenderWindowToResorucesIterator it = mMapRenderWindowToResoruces.begin();

			// Multi head device case -  
			// Present parameter index is based on adapter ordinal in group index.
			while (it != mMapRenderWindowToResoruces.end())			
			{
				it->second->presentParametersIndex = it->second->adapterOrdinalInGroupIndex;
				++it;
			}
		}
		else
		{
			// Single head device case - 
			// Just assign index in incremental order - 
			// NOTE: It suppose to cover both cases that possible here:
			// 1. Single full screen window - only one allowed per device (this is not multi-head case).
			// 2. Multiple window mode windows.

			UINT32 nextPresParamIndex = 0;

			RenderWindowToResorucesIterator it;
			D3D9RenderWindow* deviceFocusWindow = NULL;

			// In case a d3d9 device exists - try to keep the present parameters order
			// so that the window that the device is focused on will stay the same and we
			// will avoid device re-creation.
			if (mpDevice != NULL)
			{
				it = mMapRenderWindowToResoruces.begin();
				while (it != mMapRenderWindowToResoruces.end())			
				{
					if (it->first->getWindowHandle() == mCreationParams.hFocusWindow)
					{
						deviceFocusWindow = it->first;
						it->second->presentParametersIndex = nextPresParamIndex;
						++nextPresParamIndex;
						break;
					}					
					++it;
				}
			}
			
		

			it = mMapRenderWindowToResoruces.begin();
			while (it != mMapRenderWindowToResoruces.end())			
			{
				if (it->first != deviceFocusWindow)
				{
					it->second->presentParametersIndex = nextPresParamIndex;
					++nextPresParamIndex;
				}								
				++it;
			}
		}
	}
	//---------------------------------------------------------------------
	void D3D9Device::copyContentsToMemory(D3D9RenderWindow* renderWindow, 
		const PixelBox &dst, RenderTarget::FrameBuffer buffer)
	{
		RenderWindowToResorucesIterator it = getRenderWindowIterator(renderWindow);
		RenderWindowResources* resources = it->second;
		bool swapChain = isSwapChainWindow(renderWindow);



		if ((dst.left < 0) || (dst.right > renderWindow->getWidth()) ||
			(dst.top < 0) || (dst.bottom > renderWindow->getHeight()) ||
			(dst.front != 0) || (dst.back != 1))
		{
			OGRE_EXCEPT(Exception::ERR_INVALIDPARAMS,
				"Invalid box.",
				"D3D9Device::copyContentsToMemory" );
		}

		HRESULT hr;
		IDirect3DSurface9* pSurf = NULL;
		IDirect3DSurface9* pTempSurf = NULL;
		D3DSURFACE_DESC desc;
		D3DLOCKED_RECT lockedRect;


		if (buffer == RenderTarget::FB_AUTO)
		{
			//buffer = mIsFullScreen? FB_FRONT : FB_BACK;
			buffer = RenderTarget::FB_FRONT;
		}

		if (buffer == RenderTarget::FB_FRONT)
		{
			D3DDISPLAYMODE dm;

			if (FAILED(hr = mpDevice->GetDisplayMode(0, &dm)))
			{
				OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR,
					"Can't get display mode: TODO PORT NO ERROR", // TODO PORT - Translate HR to error
					"D3D9Device::copyContentsToMemory");
			}

			desc.Width = dm.Width;
			desc.Height = dm.Height;
			desc.Format = D3DFMT_A8R8G8B8;
			if (FAILED(hr = mpDevice->CreateOffscreenPlainSurface(desc.Width, desc.Height,
				desc.Format,
				D3DPOOL_SYSTEMMEM,
				&pTempSurf,
				0)))
			{
				OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR,
					"Can't create offscreen buffer: TODO PORT NO ERROR", // TODO PORT - Translate HR to error
					"D3D9Device::copyContentsToMemory");
			}

			if (FAILED(hr = swapChain ? resources->swapChain->GetFrontBufferData(pTempSurf) :
				mpDevice->GetFrontBufferData(0, pTempSurf)))
			{
				SAFE_RELEASE(pTempSurf);
				OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR,
					"Can't get front buffer: TODO PORT NO ERROR", // TODO PORT - Translate HR to error
					"D3D9Device::copyContentsToMemory");
			}

			if(renderWindow->isFullScreen())
			{
				if ((dst.left == 0) && (dst.right == renderWindow->getWidth()) && (dst.top == 0) && (dst.bottom == renderWindow->getHeight()))
				{
					hr = pTempSurf->LockRect(&lockedRect, 0, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK);
				}
				else
				{
					RECT rect;

					rect.left = static_cast<LONG>(dst.left);
					rect.right = static_cast<LONG>(dst.right);
					rect.top = static_cast<LONG>(dst.top);
					rect.bottom = static_cast<LONG>(dst.bottom);

					hr = pTempSurf->LockRect(&lockedRect, &rect, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK);
				}
				if (FAILED(hr))
				{
					SAFE_RELEASE(pTempSurf);
					OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR,
						"Can't lock rect:  TODO PORT NO ERROR", // TODO PORT - Translate HR to error
						"D3D9Device::copyContentsToMemory");
				} 
			}
			else
			{
				RECT srcRect;
				//GetClientRect(mHWnd, &srcRect);
				srcRect.left = static_cast<LONG>(dst.left);
				srcRect.top = static_cast<LONG>(dst.top);
				srcRect.right = static_cast<LONG>(dst.right);
				srcRect.bottom = static_cast<LONG>(dst.bottom);
				POINT point;
				point.x = srcRect.left;
				point.y = srcRect.top;
				ClientToScreen(renderWindow->getWindowHandle(), &point);
				srcRect.top = point.y;
				srcRect.left = point.x;
				srcRect.bottom += point.y;
				srcRect.right += point.x;

				desc.Width = srcRect.right - srcRect.left;
				desc.Height = srcRect.bottom - srcRect.top;

				if (FAILED(hr = pTempSurf->LockRect(&lockedRect, &srcRect, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK)))
				{
					SAFE_RELEASE(pTempSurf);
					OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR,
						"Can't lock rect:  TODO PORT NO ERROR", // TODO PORT - Translate HR to error
						"D3D9Device::copyContentsToMemory");
				} 
			}
		}
		else
		{
			SAFE_RELEASE(pSurf);
			if(FAILED(hr = swapChain? resources->swapChain->GetBackBuffer(0, D3DBACKBUFFER_TYPE_MONO, &pSurf) :
				mpDevice->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &pSurf)))
			{
				OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR,
					"Can't get back buffer:  TODO PORT NO ERROR", // TODO PORT - Translate HR to error
					"D3D9Device::copyContentsToMemory");
			}

			if(FAILED(hr = pSurf->GetDesc(&desc)))
			{
				OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR,
					"Can't get description:  TODO PORT NO ERROR", // TODO PORT - Translate HR to error
					"D3D9Device::copyContentsToMemory");
			}

			if (FAILED(hr = mpDevice->CreateOffscreenPlainSurface(desc.Width, desc.Height,
				desc.Format,
				D3DPOOL_SYSTEMMEM,
				&pTempSurf,
				0)))
			{
				OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR,
					"Can't create offscreen surface:  TODO PORT NO ERROR", // TODO PORT - Translate HR to error
					"D3D9Device::copyContentsToMemory");
			}

			if (desc.MultiSampleType == D3DMULTISAMPLE_NONE)
			{
				if (FAILED(hr = mpDevice->GetRenderTargetData(pSurf, pTempSurf)))
				{
					SAFE_RELEASE(pTempSurf);
					OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR,
						"Can't get render target data:  TODO PORT NO ERROR", // TODO PORT - Translate HR to error
						"D3D9Device::copyContentsToMemory");
				}
			}
			else
			{
				IDirect3DSurface9* pStretchSurf = 0;

				if (FAILED(hr = mpDevice->CreateRenderTarget(desc.Width, desc.Height,
					desc.Format,
					D3DMULTISAMPLE_NONE,
					0,
					false,
					&pStretchSurf,
					0)))
				{
					SAFE_RELEASE(pTempSurf);
					OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR,
						"Can't create render target:  TODO PORT NO ERROR", // TODO PORT - Translate HR to error
						"D3D9Device::copyContentsToMemory");
				}

				if (FAILED(hr = mpDevice->StretchRect(pSurf, 0, pStretchSurf, 0, D3DTEXF_NONE)))
				{
					SAFE_RELEASE(pTempSurf);
					SAFE_RELEASE(pStretchSurf);
					OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR,
						"Can't stretch rect:  TODO PORT NO ERROR", // TODO PORT - Translate HR to error
						"D3D9Device::copyContentsToMemory");
				}
				if (FAILED(hr = mpDevice->GetRenderTargetData(pStretchSurf, pTempSurf)))
				{
					SAFE_RELEASE(pTempSurf);
					SAFE_RELEASE(pStretchSurf);
					OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR,
						"Can't get render target data:  TODO PORT NO ERROR", // TODO PORT - Translate HR to error
						"D3D9Device::copyContentsToMemory");
				}
				SAFE_RELEASE(pStretchSurf);
			}

			if ((dst.left == 0) && (dst.right == renderWindow->getWidth()) && (dst.top == 0) && (dst.bottom == renderWindow->getHeight()))
			{
				hr = pTempSurf->LockRect(&lockedRect, 0, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK);
			}
			else
			{
				RECT rect;

				rect.left = static_cast<LONG>(dst.left);
				rect.right = static_cast<LONG>(dst.right);
				rect.top = static_cast<LONG>(dst.top);
				rect.bottom = static_cast<LONG>(dst.bottom);

				hr = pTempSurf->LockRect(&lockedRect, &rect, D3DLOCK_READONLY | D3DLOCK_NOSYSLOCK);
			}
			if (FAILED(hr))
			{
				SAFE_RELEASE(pTempSurf);
				OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR,
					"Can't lock rect:  TODO PORT NO ERROR", // TODO PORT - Translate HR to error
					"D3D9Device::copyContentsToMemory");
			}
		}

		PixelFormat format = CamelotEngine::D3D9Mappings::_getPF(desc.Format);

		if (format == PF_UNKNOWN)
		{
			SAFE_RELEASE(pTempSurf);
			OGRE_EXCEPT(Exception::ERR_RENDERINGAPI_ERROR,
				"Unsupported format", "D3D9Device::copyContentsToMemory");
		}

		PixelBox src(dst.getWidth(), dst.getHeight(), 1, format, lockedRect.pBits);
		src.rowPitch = lockedRect.Pitch / PixelUtil::getNumElemBytes(format);
		src.slicePitch = desc.Height * src.rowPitch;

		PixelUtil::bulkPixelConversion(src, dst);

		SAFE_RELEASE(pTempSurf);
		SAFE_RELEASE(pSurf);


	}
}
