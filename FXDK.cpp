#define NOMINMAX

extern "C" {
#include <ntddk.h>
#include <ntddstor.h>
#pragma warning  (push)
#pragma warning (disable : 4471)
#include <wdf.h>
#pragma warning (pop)
}

namespace meta {
	template <typename>
	constexpr bool is_pointer_v = false;

	template <typename T>
	constexpr bool is_pointer_v<T*> = true;

	template <typename T>
	constexpr bool is_pointer_v<T* const> = true;

	template <typename T>
	constexpr bool is_pointer_v<T* volatile> = true;

	template <typename T>
	constexpr bool is_pointer_v<T* const volatile> = true;
}

#define LOG(fmt, ...) do { DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "FXDK: " fmt "\n", __VA_ARGS__); } while (0)

template <typename R, typename T> requires meta::is_pointer_v<T> && meta::is_pointer_v<R> static _forceinline constexpr R cast(T p) {
	return reinterpret_cast<R>(p);
}

class WDFRequest final {
private:
	WDFREQUEST Handle;
	NTSTATUS Status;
public:
	_forceinline explicit WDFRequest(WDFREQUEST request, NTSTATUS status) noexcept : Handle(request), Status(status) {}
	_forceinline explicit WDFRequest(WDFREQUEST request) noexcept : Handle(request), Status(WdfRequestGetStatus(Handle)) {}
	WDFRequest(const WDFRequest&) = delete;
	WDFRequest(WDFRequest&&) = delete;
	WDFRequest& operator=(const WDFRequest&) = delete;
	WDFRequest& operator=(WDFRequest&&) noexcept = delete;
	_forceinline void SetStatus(NTSTATUS status) noexcept {
		Status = status;
	}
	[[nodiscard]] _forceinline NTSTATUS GetStatus() const noexcept {
		return Status;
	}
	_forceinline ~WDFRequest() noexcept {
		if (Handle != WDF_NO_HANDLE) {
			WdfRequestComplete(Handle, Status);
		}
	}
	_forceinline void Take() noexcept {
		Handle = WDF_NO_HANDLE;
	}
	// NOLINTNEXTLINE(google-explicit-constructor)
	_forceinline explicit(false) operator WDFREQUEST() const noexcept {
		return Handle;
	}
};

_Function_class_(EVT_WDF_DEVICE_PREPARE_HARDWARE)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
static
NTSTATUS
EventDevicePrepareHardware(
	_In_
	WDFDEVICE Device,
	_In_
	WDFCMRESLIST ResourcesRaw,
	_In_
	WDFCMRESLIST ResourcesTranslated
) {
	UNREFERENCED_PARAMETER(ResourcesRaw);
	UNREFERENCED_PARAMETER(ResourcesTranslated);
	LOG("EvtPrepareHardware: Hack FDO Characteristics");
	WdfDeviceSetCharacteristics(Device, WdfDeviceGetCharacteristics(Device) & ~FILE_REMOVABLE_MEDIA);
	LOG("EvtPrepareHardware: FDO Characteristics after just clear: 0x%08X", WdfDeviceGetCharacteristics(Device));
	return STATUS_SUCCESS;
}

_Function_class_(EVT_WDF_REQUEST_COMPLETION_ROUTINE)
_IRQL_requires_same_
static
VOID
RemovablePropertyFilterCompletionRoutine(
	_In_
	WDFREQUEST _Request,
	_In_
	WDFIOTARGET Target,
	_In_
	PWDF_REQUEST_COMPLETION_PARAMS Params,
	_In_
	WDFCONTEXT Context
) {
	UNREFERENCED_PARAMETER(Context);
	UNREFERENCED_PARAMETER(Params);
	UNREFERENCED_PARAMETER(Target);

	WDFRequest Request(_Request);
	if (NT_SUCCESS(Request.GetStatus())) {
		STORAGE_DEVICE_DESCRIPTOR* Descriptor = nullptr;
		size_t OutputBufferSize = 0;
		auto status = WdfRequestRetrieveOutputBuffer(Request, sizeof(*Descriptor), cast<PVOID*>(&Descriptor), &OutputBufferSize);
		if (NT_SUCCESS(status) && OutputBufferSize >= sizeof(*Descriptor)) {
			Descriptor->RemovableMedia = FALSE;
			LOG("Clear RemovableMedia in STORAGE_DEVICE_DESCRIPTOR");
		}
		else {
			LOG("Failed to retrieve STORAGE_DEVICE_DESCRIPTOR output buffer from request in completion routine, OutputBufferSize = %zX, status = 0x%08X", OutputBufferSize, status);
		}

	}
}

_Function_class_(EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL)
_IRQL_requires_same_
_IRQL_requires_max_(DISPATCH_LEVEL)
static
VOID
EventIoDeviceControl(
	_In_ WDFQUEUE Queue,
	_In_ WDFREQUEST _Request,
	_In_ size_t OutputBufferLength,
	_In_ size_t InputBufferLength,
	_In_ ULONG IoControlCode
)
{
	//LOG("Received IOCTL request, code = 0x%08X, request = %p, OutputBufferLength = %zX, InputBufferLength = %zX", IoControlCode, Request, OutputBufferLength, InputBufferLength);;

	WDFRequest Request(_Request, STATUS_UNSUCCESSFUL);
	WDFDEVICE Device = WdfIoQueueGetDevice(Queue);
	if (Device == nullptr) {
		LOG("Failed to get WDF device object from queue object");
		return;
	}
	WDFIOTARGET Target = WdfDeviceGetIoTarget(Device);
	if (Target == nullptr) {
		LOG("Failed to get WDF local IO target device from WDF device object");
		return;
	}

	WDF_REQUEST_SEND_OPTIONS RequestSendOptions;

	WDF_REQUEST_SEND_OPTIONS_INIT(&RequestSendOptions, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
	// Ignore query without enough output buffer
	if (IoControlCode == IOCTL_STORAGE_QUERY_PROPERTY && OutputBufferLength >= sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
		size_t RetrievedInputBufferLength = InputBufferLength;
		STORAGE_PROPERTY_QUERY* Descriptor = nullptr;
		auto status = WdfRequestRetrieveInputBuffer(Request, sizeof(STORAGE_PROPERTY_QUERY), cast<PVOID*>(&Descriptor), &RetrievedInputBufferLength);
		if (NT_SUCCESS(status) && RetrievedInputBufferLength >= InputBufferLength) {
			if (Descriptor->PropertyId == StorageDeviceProperty && Descriptor->QueryType == PropertyStandardQuery) {
				LOG("IOCTL_STORAGE_QUERY_PROPERTY query type is PropertyStandardQuery, will set completion routine");
				RequestSendOptions.Flags &= ~WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET;
				WdfRequestSetCompletionRoutine(Request, RemovablePropertyFilterCompletionRoutine, nullptr);
			}
		}
		else {
			LOG("Failed to retrieve STORAGE_PROPERTY_QUERY descriptor from InputBuffer, RetrievedInputBufferLength = %zX, status = 0x%08X", RetrievedInputBufferLength, status);
		}
	}
	WdfRequestFormatRequestUsingCurrentType(Request);
	if (!WdfRequestSend(Request, Target, &RequestSendOptions)) {
		Request.SetStatus(WdfRequestGetStatus(Request));
		LOG("Failed to send formated IOCTL request to underlying driver, status = 0x%08X", Request.GetStatus());
	}
	else {
		Request.Take();
	}
}

_Function_class_(EVT_WDF_DEVICE_RELEASE_HARDWARE)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
static
NTSTATUS
EventDeviceReleaseHardware(
	_In_
	WDFDEVICE Device,
	_In_
	WDFCMRESLIST ResourcesTranslated
) {
	UNREFERENCED_PARAMETER(Device);
	UNREFERENCED_PARAMETER(ResourcesTranslated);
	ULONG characteristics = WdfDeviceGetCharacteristics(Device);
	LOG("EvtDeviceReleaseHardware: FDO Characteristics: 0x%08X", characteristics);
	return STATUS_SUCCESS;
}

_Function_class_(EVT_WDF_DRIVER_DEVICE_ADD)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
static
NTSTATUS
EventDeviceAdd(
	_In_    WDFDRIVER       Driver,
	_Inout_ PWDFDEVICE_INIT DeviceInit
)
{
	UNREFERENCED_PARAMETER(Driver);

	PAGED_CODE();

	NTSTATUS status = STATUS_SUCCESS;

	PDEVICE_OBJECT PDO = WdfFdoInitWdmGetPhysicalDevice(DeviceInit);
	if (PDO == nullptr) {
		LOG("Unable to get underlying PDO object");
		return status;
	}

	if ((PDO->Characteristics & FILE_REMOVABLE_MEDIA) == 0) {
		LOG("underlying PDO object is not removable");
		return status;
	}

	WdfFdoInitSetFilter(DeviceInit);

	LOG("Detected removable PDO, attach to stack");

	WDF_PNPPOWER_EVENT_CALLBACKS PNPCallbacks;

	WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&PNPCallbacks);
	PNPCallbacks.EvtDevicePrepareHardware = EventDevicePrepareHardware;
	PNPCallbacks.EvtDeviceReleaseHardware = EventDeviceReleaseHardware;

	WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &PNPCallbacks);

	WDFDEVICE Device;
	status = WdfDeviceCreate(&DeviceInit, WDF_NO_OBJECT_ATTRIBUTES, &Device);

	if (NT_SUCCESS(status)) {
		// Prevent propagating FILE_REMOVABLE_MEDIA flag to upper layer device objects. The flag will be set again by
		// WDF framework after PNP_MN_START_DEVICE is triggered then we clear it again in EventDevicePrepareHardware.
		WdfDeviceSetCharacteristics(Device, WdfDeviceGetCharacteristics(Device) & ~FILE_REMOVABLE_MEDIA);
		LOG("Try to clear removable device characteristic before propagation");
		WDF_IO_QUEUE_CONFIG QueueConfig;
		WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(
			&QueueConfig,
			WdfIoQueueDispatchParallel
		);

		QueueConfig.EvtIoDeviceControl = EventIoDeviceControl;
		QueueConfig.DefaultQueue = TRUE;
		QueueConfig.PowerManaged = WdfFalse;

		WDFQUEUE Queue;

		status = WdfIoQueueCreate(
			Device,
			&QueueConfig,
			WDF_NO_OBJECT_ATTRIBUTES,
			&Queue
		);
		if (!NT_SUCCESS(status)) {
			LOG("Failed to create queue object for WDF device object, status = 0x%08X", status);
		}
		else {
			LOG("EventDeviceAdd: FDO characteristics: 0x%08X", WdfDeviceGetCharacteristics(Device));
		}
	}
	else {
		LOG("Failed to create WDF device object, status = 0x%08X", status);
	}

	return status;
}

_Function_class_(EVT_WDF_DRIVER_UNLOAD)
_IRQL_requires_same_
_IRQL_requires_max_(PASSIVE_LEVEL)
static
VOID
EventDriverUnload(
	_In_
	WDFDRIVER Driver
) {
	UNREFERENCED_PARAMETER(Driver);
	LOG("On driver unload");
}

extern "C" {
	DRIVER_INITIALIZE DriverEntry;
	NTSTATUS
		DriverEntry(
			_In_ PDRIVER_OBJECT  DriverObject,
			_In_ PUNICODE_STRING RegistryPath
		) {
		WDF_DRIVER_CONFIG Config;
		NTSTATUS status;
		WDF_OBJECT_ATTRIBUTES Attributes;
		LOG("On driver load");

		WDF_OBJECT_ATTRIBUTES_INIT(&Attributes);
		WDF_DRIVER_CONFIG_INIT(&Config,
			EventDeviceAdd
		);
		Config.EvtDriverUnload = EventDriverUnload;
		status = WdfDriverCreate(DriverObject,
			RegistryPath,
			&Attributes,
			&Config,
			WDF_NO_HANDLE
		);

		if (!NT_SUCCESS(status)) {
			LOG("Failed to create WDF driver object, status = 0x%08X", status);
		}
		else {
			LOG("On driver object created");
		}

		return status;
	}
}