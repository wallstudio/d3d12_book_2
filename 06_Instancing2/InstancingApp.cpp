#include "InstancingApp.h"
#include "imgui_helper.h"
#include <random>

#include "TeapotModel.h"

#include "imgui.h"
#include "examples/imgui_impl_dx12.h"
#include "examples/imgui_impl_win32.h"

using namespace DirectX;

static DirectX::XMFLOAT4 colorSet[] = {
  XMFLOAT4(1.0f, 1.0f, 1.0f, 1.0f),
  XMFLOAT4(1.0f, 0.65f, 1.0f, 1.0f),
  XMFLOAT4(0.1f, 0.5f, 1.0f, 1.0f),
  XMFLOAT4(0.6f, 1.0f, 0.8f, 1.0f),
};

InstancingApp::InstancingApp()
  :m_instancingCount(100), m_cameraOffset(0.0f)
{
}

void InstancingApp::Prepare()
{
  SetTitle("Instancing : Use SV_InstanceID");
  // バッファの転送を行うためにコマンドリストを使うので準備する.
  m_commandAllocators[m_frameIndex]->Reset();
  m_commandList->Reset(m_commandAllocators[m_frameIndex].Get(), nullptr);

  void* mapped;
  HRESULT hr;
  CD3DX12_RANGE range(0, 0);
  UINT bufferSize = sizeof(TeapotModel::TeapotVerticesPN);

  m_model.resourceVB = CreateBufferResource(
    D3D12_HEAP_TYPE_DEFAULT, bufferSize, D3D12_RESOURCE_STATE_COPY_DEST
  );

  auto uploadVB = CreateBufferResource(
    D3D12_HEAP_TYPE_UPLOAD, bufferSize, D3D12_RESOURCE_STATE_GENERIC_READ
  );

  hr = uploadVB->Map(0, nullptr, &mapped);
  if (SUCCEEDED(hr)) {
    memcpy(mapped, TeapotModel::TeapotVerticesPN, bufferSize);
    uploadVB->Unmap(0, nullptr);
  }
  m_model.vbView.BufferLocation = m_model.resourceVB->GetGPUVirtualAddress();
  m_model.vbView.SizeInBytes = bufferSize;
  m_model.vbView.StrideInBytes = sizeof(TeapotModel::Vertex);

  m_commandList->CopyResource(m_model.resourceVB.Get(), uploadVB.Get());

  bufferSize = sizeof(TeapotModel::TeapotIndices);
  m_model.resourceIB = CreateBufferResource(
    D3D12_HEAP_TYPE_DEFAULT, bufferSize, D3D12_RESOURCE_STATE_COPY_DEST
  );
  auto uploadIB = CreateBufferResource(
    D3D12_HEAP_TYPE_UPLOAD, bufferSize, D3D12_RESOURCE_STATE_GENERIC_READ
  );

  hr = uploadIB->Map(0, nullptr, &mapped);
  if (SUCCEEDED(hr)) {
    memcpy(mapped, TeapotModel::TeapotIndices, bufferSize);
    uploadIB->Unmap(0, nullptr);
  }
  m_model.ibView.BufferLocation = m_model.resourceIB->GetGPUVirtualAddress();
  m_model.ibView.SizeInBytes = bufferSize;
  m_model.ibView.Format = DXGI_FORMAT_R32_UINT;
  m_model.indexCount = _countof(TeapotModel::TeapotIndices);

  m_commandList->CopyResource(m_model.resourceIB.Get(), uploadIB.Get());

  // コピー処理が終わった後は各バッファのステートを適切に変更しておく.
  auto barrierVB = CD3DX12_RESOURCE_BARRIER::Transition(
    m_model.resourceVB.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
  );
  auto barrierIB = CD3DX12_RESOURCE_BARRIER::Transition(
    m_model.resourceIB.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_INDEX_BUFFER
  );
  D3D12_RESOURCE_BARRIER barriers[] = {
    barrierVB, barrierIB,
  };
  m_commandList->ResourceBarrier(_countof(barriers), barriers);

  m_commandList->Close();
  ID3D12CommandList* command[] = { m_commandList.Get() };
  m_commandQueue->ExecuteCommandLists(1, command);

  WaitForIdleGPU(); // 処理が完了するのを待つ.

  ComPtr<ID3DBlob> errBlob;
  hr = CompileShaderFromFile(
    L"VertexShader.hlsl", L"vs_6_0", m_vs, errBlob);
  ThrowIfFailed(hr, "VertexShader error");

  hr = CompileShaderFromFile(
    L"PixelShader.hlsl", L"ps_6_0", m_ps, errBlob);
  ThrowIfFailed(hr, "PixelShader error");

  // ルートシグネチャ構築
  CD3DX12_ROOT_PARAMETER rootParams[2];
  rootParams[0].InitAsConstantBufferView(0);
  rootParams[1].InitAsConstantBufferView(1);
  CD3DX12_ROOT_SIGNATURE_DESC rootSigDesc{};
  rootSigDesc.Init(
    _countof(rootParams), rootParams,
    0, nullptr,
    D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
  ComPtr<ID3DBlob> signature;
  hr = D3D12SerializeRootSignature(&rootSigDesc, D3D_ROOT_SIGNATURE_VERSION_1_0, &signature, &errBlob);
  ThrowIfFailed(hr, "D3D12SerializeRootSignature failed.");
  hr = m_device->CreateRootSignature(
    0, signature->GetBufferPointer(), signature->GetBufferSize(),
    IID_PPV_ARGS(&m_rootSignature)
  );
  ThrowIfFailed(hr, "CreateRootSignature failed.");

  // インプットレイアウト
  D3D12_INPUT_ELEMENT_DESC inputElementDesc[] = {
    { "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(TeapotModel::Vertex, Position), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA},
    { "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT,0, offsetof(TeapotModel::Vertex,Normal), D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA}
  };

  // パイプラインステートオブジェクトの生成.
  D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc{};
  // シェーダーのセット
  psoDesc.VS = CD3DX12_SHADER_BYTECODE(m_vs.Get());
  psoDesc.PS = CD3DX12_SHADER_BYTECODE(m_ps.Get());
  // ブレンドステート設定
  psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
  // ラスタライザーステート
  psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
  psoDesc.RasterizerState.FrontCounterClockwise = true;

  // 出力先は1ターゲット
  psoDesc.NumRenderTargets = 1;
  psoDesc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
  // デプスバッファのフォーマットを設定
  psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
  psoDesc.DepthStencilState = CD3DX12_DEPTH_STENCIL_DESC(D3D12_DEFAULT);
  psoDesc.InputLayout = { inputElementDesc, _countof(inputElementDesc) };

  // ルートシグネチャのセット
  psoDesc.pRootSignature = m_rootSignature.Get();
  psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  // マルチサンプル設定
  psoDesc.SampleDesc = { 1,0 };
  psoDesc.SampleMask = UINT_MAX; // これを忘れると絵が出ない＆警告も出ないので注意.

  hr = m_device->CreateGraphicsPipelineState(&psoDesc, IID_PPV_ARGS(&m_pipeline));
  ThrowIfFailed(hr, "CreateGraphicsPipelineState failed.");

  // 定数バッファの準備
  bufferSize = book_util::RoundupConstantBufferSize(sizeof(SceneParameter));
  for (UINT i = 0; i < FrameBufferCount; ++i) {
    auto cb = CreateBufferResource(
      D3D12_HEAP_TYPE_UPLOAD,
      bufferSize,
      D3D12_RESOURCE_STATE_GENERIC_READ
    );
    m_constantBuffers.push_back(cb);
  }

  std::vector<InstanceData> instanceData(InstanceDataMax);
  {
    std::random_device rnd;
    // インスタンシング中身を書き込んでおく.
    for (UINT i = 0; i < InstanceDataMax; ++i)
    {
      float k = float(rnd() % 360);
      float x = (i % 6) * 3.0f;
      float z = (i / 6) * -3.0f;

      auto t0 = XMMatrixTranslation(x, 0.0f, z);
      auto m0 = XMMatrixRotationZ(k);
      auto m1 = XMMatrixRotationX(k);

      auto& data = instanceData[i];
      XMStoreFloat4x4(&data.world, XMMatrixTranspose(m0 * m1 * t0));
      data.Color = colorSet[i % _countof(colorSet)];
    }
  }

  // インスタンシング用のバッファを準備.
  bufferSize = sizeof(InstanceData) * InstanceDataMax;
  bufferSize = book_util::RoundupConstantBufferSize(bufferSize);
  for (UINT i = 0; i < FrameBufferCount; ++i)
  {
    auto cb = CreateBufferResource(
      D3D12_HEAP_TYPE_UPLOAD,
      bufferSize,
      D3D12_RESOURCE_STATE_GENERIC_READ
    );
    m_instanceBuffers.push_back(cb);

    cb->Map(0, nullptr, &mapped);
    memcpy(mapped, instanceData.data(), bufferSize);
    cb->Unmap(0, nullptr);
  }

  // ImGui セットアップ
  auto descriptor = m_heap->Alloc();
  CD3DX12_CPU_DESCRIPTOR_HANDLE hCpu(descriptor);
  CD3DX12_GPU_DESCRIPTOR_HANDLE hGpu(descriptor);

  imgui_helper::PrepareImGui(
    m_hwnd,
    m_device.Get(),
    m_surfaceFormat,
    FrameBufferCount,
    hCpu, hGpu
  );
}
void InstancingApp::Cleanup()
{
  imgui_helper::CleanupImGui();
}


void InstancingApp::Render()
{
  UpdateImGui();

  SceneParameter sceneParam;
  XMStoreFloat4x4(&sceneParam.view,
    XMMatrixTranspose(
      XMMatrixLookAtRH(
        XMVectorSet(3.0f, 5.0f, 10.0f - m_cameraOffset, 0.0f),
        XMVectorSet(3.0f, 2.0f, 0.0f - m_cameraOffset, 0.0f),
        XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f)))
  );
  XMStoreFloat4x4(&sceneParam.proj,
    XMMatrixTranspose(
      XMMatrixPerspectiveFovRH(XMConvertToRadians(45.0f), m_viewport.Width / m_viewport.Height, 0.1f, 1000.0f)
    )
  );


  m_frameIndex = m_swapchain->GetCurrentBackBufferIndex();

  m_commandAllocators[m_frameIndex]->Reset();
  m_commandList->Reset(
    m_commandAllocators[m_frameIndex].Get(),
    nullptr
  );

  // スワップチェイン表示可能からレンダーターゲット描画可能へ
  auto barrierToRT = m_swapchain->GetBarrierToRenderTarget();
  m_commandList->ResourceBarrier(1, &barrierToRT);

  auto rtv = m_swapchain->GetCurrentRTV();
  auto dsv = m_defaultDepthDSV;

  // カラーバッファ(レンダーターゲットビュー)のクリア
  float clearColor[4] = { 0.5f,0.75f,1.0f,0 };
  m_commandList->ClearRenderTargetView(rtv, clearColor, 0, nullptr);

  // デプスバッファ(デプスステンシルビュー)のクリア
  m_commandList->ClearDepthStencilView(
    dsv, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr);

  // 描画先をセット
  m_commandList->OMSetRenderTargets(1, &(D3D12_CPU_DESCRIPTOR_HANDLE)rtv,
    FALSE, &(D3D12_CPU_DESCRIPTOR_HANDLE)dsv);

  auto viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, float(m_width), float(m_height));
  auto scissorRect = CD3DX12_RECT(0, 0, LONG(m_width), LONG(m_height));

  // ビューポートとシザーのセット
  m_commandList->RSSetViewports(1, &viewport);
  m_commandList->RSSetScissorRects(1, &scissorRect);

  ID3D12DescriptorHeap* heaps[] = { m_heap->GetHeap().Get() };
  m_commandList->SetDescriptorHeaps(_countof(heaps), heaps);

  auto cb = m_constantBuffers[m_frameIndex];
  {
    void* mapped;
    cb->Map(0, nullptr, &mapped);
    memcpy(mapped, &sceneParam, sizeof(sceneParam));
    cb->Unmap(0, nullptr);
  }
  auto instanceCb = m_instanceBuffers[m_frameIndex];

  m_commandList->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  m_commandList->IASetVertexBuffers(0, 1, &m_model.vbView);
  m_commandList->IASetIndexBuffer(&m_model.ibView);

  m_commandList->SetGraphicsRootSignature(m_rootSignature.Get());
  m_commandList->SetPipelineState(m_pipeline.Get());
  m_commandList->SetGraphicsRootConstantBufferView(
    0, cb->GetGPUVirtualAddress()
  );
  m_commandList->SetGraphicsRootConstantBufferView(
    1, instanceCb->GetGPUVirtualAddress()
  );

  m_commandList->DrawIndexedInstanced(
    m_model.indexCount,
    m_instancingCount,
    0, 0, 0
  );

  RenderImGui();

  // レンダーターゲットからスワップチェイン表示可能へ
  auto barrierToPresent = m_swapchain->GetBarrierToPresent();
  m_commandList->ResourceBarrier(1, &barrierToPresent);

  m_commandList->Close();

  ID3D12CommandList* lists[] = { m_commandList.Get() };

  m_commandQueue->ExecuteCommandLists(1, lists);

  m_swapchain->Present(1, 0);
  m_swapchain->WaitPreviousFrame(m_commandQueue, m_frameIndex, GpuWaitTimeout);
}

void InstancingApp::UpdateImGui()
{
  ImGui_ImplDX12_NewFrame();
  ImGui_ImplWin32_NewFrame();
  ImGui::NewFrame();

  auto framerate = ImGui::GetIO().Framerate;
  ImGui::Begin("Control");
  ImGui::Text("Using SV_InstanceID");
  ImGui::Text("Framerate(avg) %.3f ms/frame", 1000.0f / framerate);
  ImGui::SliderInt("Count", &m_instancingCount, 1, InstanceDataMax);
  ImGui::SliderFloat("Camera", &m_cameraOffset, 0.0f, 50.0f);
  ImGui::End();
}

void InstancingApp::RenderImGui()
{
  ImGui::Render();
  ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), m_commandList.Get());
}

InstancingApp::Buffer InstancingApp::CreateBufferResource(D3D12_HEAP_TYPE type, UINT bufferSize, D3D12_RESOURCE_STATES state)
{
  Buffer ret;
  HRESULT hr;

  hr = m_device->CreateCommittedResource(
    &CD3DX12_HEAP_PROPERTIES(type),
    D3D12_HEAP_FLAG_NONE,
    &CD3DX12_RESOURCE_DESC::Buffer(bufferSize),
    state,
    nullptr,
    IID_PPV_ARGS(&ret)
  );
  ThrowIfFailed(hr, "CreateCommittedResource failed.");
  return ret;
}