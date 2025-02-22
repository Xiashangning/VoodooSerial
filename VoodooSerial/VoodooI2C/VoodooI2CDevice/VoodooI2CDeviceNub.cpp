//
//  VoodooI2CDeviceNub.cpp
//  VoodooI2C
//
//  Created by Alexandre on 07/08/2017.
//  Copyright © 2017 Alexandre Daoud. All rights reserved.
//

#include "../VoodooI2CController/VoodooI2CControllerDriver.hpp"
#include "VoodooI2CDeviceNub.hpp"

#include <Availability.h>

//#ifndef __ACIDANTHERA_MAC_SDK
//#error "This kext SDK is unsupported. Download from https://github.com/acidanthera/MacKernelSDK"
//#endif

#define super IOService
OSDefineMetaClassAndStructors(VoodooI2CDeviceNub, IOService);

bool VoodooI2CDeviceNub::attach(IOService* provider, IOService* child) {
    if (!super::attach(provider))
        return false;

    controller_name = provider->getName();
    setProperty("acpi-device", child);
    acpi_device = OSDynamicCast(IOACPIPlatformDevice, child);

    if (!acpi_device) {
        IOLog("%s::%s Could not get ACPI device\n", controller_name, child->getName());
        return false;
    }

    controller = OSDynamicCast(VoodooI2CControllerDriver, provider);

    if (!controller) {
        IOLog("%s::%s Could not get controller\n", controller_name, child->getName());
        return false;
    }

    if (getDeviceResources() != kIOReturnSuccess) {
        IOLog("%s::%s Could not get device resources\n", controller_name, child->getName());
        return false;
    }

    if (has_gpio_interrupts) {
        gpio_controller = getGPIOController();

        if (!gpio_controller) {
            IOLog("%s::%s Could not find GPIO controller, exiting", controller_name, child->getName());
            return false;
        }
        // Give the GPIO controller some time to load
        IOSleep(100);
    }

    setName(child->getName());

    return true;
}

IOReturn VoodooI2CDeviceNub::disableInterrupt(int source) {
    if (has_gpio_interrupts) {
        return gpio_controller->disableInterrupt(gpio_pin);
    } else {
        return acpi_device->disableInterrupt(source);
    }
}

IOReturn VoodooI2CDeviceNub::enableInterrupt(int source) {
    if (has_gpio_interrupts) {
        return gpio_controller->enableInterrupt(gpio_pin);
    } else {
        return acpi_device->enableInterrupt(source);
    }
}

IOReturn VoodooI2CDeviceNub::evaluateDSM(const char *uuid, UInt32 index, OSObject **result) {
    IOReturn ret;
    uuid_t guid;
    uuid_parse(uuid, guid);

    // convert to mixed-endian
    *(reinterpret_cast<UInt32 *>(guid)) = OSSwapInt32(*(reinterpret_cast<UInt32 *>(guid)));
    *(reinterpret_cast<UInt16 *>(guid) + 2) = OSSwapInt16(*(reinterpret_cast<UInt16 *>(guid) + 2));
    *(reinterpret_cast<UInt16 *>(guid) + 3) = OSSwapInt16(*(reinterpret_cast<UInt16 *>(guid) + 3));

    OSObject *params[] = {
        OSData::withBytes(guid, 16),
        OSNumber::withNumber(I2C_DSM_REVISION, 32),
        OSNumber::withNumber(index, 32),
        OSArray::withCapacity(1),
    };

    ret = acpi_device->evaluateObject("XDSM", result, params, 4);
    if (ret != kIOReturnSuccess) {
        ret = acpi_device->evaluateObject("_DSM", result, params, 4);
    } else {
        IOLog("%s::%s Warning: support for XDSM method is deprecated and will be removed in a future release, please remove related rename.\n", controller_name, getName());
    }

    params[0]->release();
    params[1]->release();
    params[2]->release();
    params[3]->release();
    return ret;
}

IOReturn VoodooI2CDeviceNub::getDeviceResourcesDSM(UInt32 index, OSObject **result) {
    if (evaluateDSM(I2C_DSM_TP7G, DSM_SUPPORT_INDEX, result) != kIOReturnSuccess) {
        IOLog("%s::%s Could not find suitable _DSM or XDSM method\n", getName(), acpi_device->getName());
        return kIOReturnNotFound;
    }

    OSData *data = OSDynamicCast(OSData, *result);

    if (!data) {
        IOLog("%s::%s Could not get valid return for available TP7G indexes\n", getName(), acpi_device->getName());
        return kIOReturnNotFound;
    }

    UInt8 availableIndex = *(reinterpret_cast<UInt8 const*>(data->getBytesNoCopy()));
    OSSafeReleaseNULL(data);
    *result = nullptr;

    if (!(availableIndex & (1 << index))) {
        IOLog("%s::%s Returned index 0x%x from _DSM or XDSM method is not supported\n", getName(), acpi_device->getName(), availableIndex);
        return kIOReturnUnsupportedMode;
    }

    return evaluateDSM(I2C_DSM_TP7G, index, result);
}

IOReturn VoodooI2CDeviceNub::parseResourcesCRS(VoodooACPIResourcesParser* res_parser) {
    OSObject *result = nullptr;
    OSData *data = nullptr;
    if (acpi_device->evaluateObject("_CRS", &result) != kIOReturnSuccess ||
        !(data = OSDynamicCast(OSData, result))) {
        IOLog("%s::%s Could not find or evaluate _CRS method\n", getName(), acpi_device->getName());
        OSSafeReleaseNULL(result);
        return kIOReturnNotFound;
    }

    UInt8 const* crs = reinterpret_cast<UInt8 const*>(data->getBytesNoCopy());
    res_parser->parseACPIResources(crs, 0, data->getLength());

    OSSafeReleaseNULL(data);

    IOLog("%s::%s Found valid resources from _CRS method\n", getName(), acpi_device->getName());

    return kIOReturnSuccess;
}

IOReturn VoodooI2CDeviceNub::parseResourcesDSM(VoodooACPIResourcesParser* res_parser) {
    OSObject *result = nullptr;
    OSData *data = nullptr;
    if (getDeviceResourcesDSM(TP7G_RESOURCES_INDEX, &result) != kIOReturnSuccess ||
        !(data = OSDynamicCast(OSData, result))) {
        IOLog("%s::%s Could not retrieve resources from _DSM or XDSM method\n", getName(), acpi_device->getName());
        OSSafeReleaseNULL(result);
        return kIOReturnNotFound;
    }

    UInt8 const* crs = reinterpret_cast<UInt8 const*>(data->getBytesNoCopy());
    res_parser->parseACPIResources(crs, 0, data->getLength());

    OSSafeReleaseNULL(data);

    IOLog("%s::%s Found valid resources from _DSM or XDSM method\n", getName(), acpi_device->getName());
    return kIOReturnSuccess;
}

IOReturn VoodooI2CDeviceNub::validateAPICInterrupt() {
    OSArray* interrupt_array;
    OSData* interrupt_data;
    const UInt16* interrupt_pin;
    if ((interrupt_array = OSDynamicCast(OSArray, acpi_device->getProperty(gIOInterruptSpecifiersKey))) &&
        (interrupt_data = OSDynamicCast(OSData, interrupt_array->getObject(0))) &&
        (interrupt_pin = reinterpret_cast<const UInt16*>(interrupt_data->getBytesNoCopy(0, 1)))) {
        if (*interrupt_pin <= 0x2f) {
            has_apic_interrupts = true;
            IOLog("%s::%s Found valid APIC interrupt pin (0x%x)\n", getName(), acpi_device->getName(), *interrupt_pin);
            return kIOReturnSuccess;
        }
        IOLog("%s::%s Warning: Incompatible APIC interrupt pin (0x%x > 0x2f)\n", getName(), acpi_device->getName(), *interrupt_pin);
    }
    return kIOReturnUnsupported;
}

IOReturn VoodooI2CDeviceNub::getDeviceResources() {
    VoodooACPIResourcesParser crs_parser, dsm_parser, resource_parser;

    parseResourcesCRS(&crs_parser);
    parseResourcesDSM(&dsm_parser);

    if (!crs_parser.found_i2c && !dsm_parser.found_i2c) {
        IOLog("%s::%s Could not find an I2C Serial Bus declaration\n", getName(), acpi_device->getName());
        return kIOReturnNotFound;
    }

    bool use_crs_resources = true;

    if (!crs_parser.found_i2c || (dsm_parser.found_i2c && dsm_parser.found_gpio_interrupts)) {
        IOLog("%s::%s Prefer resources from _DSM or XDSM method\n", getName(), acpi_device->getName());
        use_crs_resources = false;
    }

    resource_parser = use_crs_resources ? crs_parser : dsm_parser;

    use_10bit_addressing = resource_parser.i2c_info.address_mode_10Bit;
    setProperty("addrWidth", use_10bit_addressing ? 10 : 7, 8);

    i2c_address = resource_parser.i2c_info.address;
    setProperty("i2cAddress", i2c_address, 16);

    setProperty("sclHz", resource_parser.i2c_info.bus_speed, 32);

    // There is actually no way to avoid APIC interrupt if it is valid
    if (validateAPICInterrupt() == kIOReturnSuccess)
        return kIOReturnSuccess;

    IOPCIDevice *pci_device { nullptr };

    pci_device = controller->nub->controller->physical_device.pci_device;

    bool force_polling = checkKernelArg("-vi2c-force-polling");
    force_polling = force_polling || ((pci_device != nullptr) && (pci_device->getProperty("force-polling") != nullptr));

    if (force_polling) {
        IOLog("%s::%s Forced polling mode, skipping GPIO interrupts\n", getName(), acpi_device->getName());
        return kIOReturnSuccess;
    }

    if (resource_parser.found_gpio_interrupts) {
        IOLog("%s::%s Found valid GPIO interrupts\n", getName(), acpi_device->getName());

        setProperty("gpioPin", resource_parser.gpio_interrupts.pin_number, 16);
        setProperty("gpioIRQ", resource_parser.gpio_interrupts.irq_type, 16);

        has_gpio_interrupts = true;
        gpio_pin = resource_parser.gpio_interrupts.pin_number;
        gpio_irq = resource_parser.gpio_interrupts.irq_type;
    }

    if (!has_apic_interrupts && !has_gpio_interrupts)
        IOLog("%s::%s Warning: Could not find any APIC nor GPIO interrupts. Your chosen satellite will run in polling mode if implemented.\n", getName(), acpi_device->getName());

    return kIOReturnSuccess;
}

VoodooGPIO* VoodooI2CDeviceNub::getGPIOController() {
    VoodooGPIO* gpio_controller = NULL;

    // Wait for GPIO controller, up to 1 second
    OSDictionary* name_match = IOService::serviceMatching("VoodooGPIO");

    IOService* matched = waitForMatchingService(name_match, 1000000000);
    gpio_controller = OSDynamicCast(VoodooGPIO, matched);

    if (gpio_controller != NULL) {
        IOLog("%s::%s Got GPIO Controller! %s\n", getName(), acpi_device->getName(), gpio_controller->getName());
    }
    name_match->release();
    OSSafeReleaseNULL(matched);

    return gpio_controller;
}

IOReturn VoodooI2CDeviceNub::getInterruptType(int source, int* interrupt_type) {
    if (has_gpio_interrupts) {
        return gpio_controller->getInterruptType(gpio_pin, interrupt_type);
    } else {
        return acpi_device->getInterruptType(source, interrupt_type);
    }
}

IOWorkLoop* VoodooI2CDeviceNub::getWorkLoop(void) const {
    return work_loop;
}

IOReturn VoodooI2CDeviceNub::readI2C(UInt8* values, UInt16 length) {
    return command_gate->attemptAction(OSMemberFunctionCast(IOCommandGate::Action, this, &VoodooI2CDeviceNub::readI2CGated), values, &length);
}

IOReturn VoodooI2CDeviceNub::readI2CGated(UInt8* values, UInt16* length) {
    UInt16 flags = I2C_M_RD;

    if (use_10bit_addressing)
        flags |= I2C_M_TEN;
    VoodooI2CControllerBusMessage msgs[] = {
        {
            .address = i2c_address,
            .buffer = values,
            .flags = flags,
            .length = *length,
        },
    };

    return controller->transferI2C(msgs, 1);
}

IOReturn VoodooI2CDeviceNub::registerInterrupt(int source, OSObject *target, IOInterruptAction handler, void *refcon) {
    if (has_gpio_interrupts) {
        gpio_controller->setInterruptTypeForPin(gpio_pin, gpio_irq);
        return gpio_controller->registerInterrupt(gpio_pin, target, handler, refcon);
    } else {
        return acpi_device->registerInterrupt(source, target, handler, refcon);
    }
    return kIOReturnSuccess;
}

void VoodooI2CDeviceNub::releaseResources() {
    if (command_gate) {
        command_gate->disable();
        work_loop->removeEventSource(command_gate);
        OSSafeReleaseNULL(command_gate);
    }

    OSSafeReleaseNULL(work_loop);
}

bool VoodooI2CDeviceNub::start(IOService* provider) {
    OSData* name;
    
    if (!super::start(provider))
        return false;

    work_loop = IOWorkLoop::workLoop();

    if (!work_loop) {
        IOLog("%s Could not get work loop\n", getName());
        goto exit;
    }

    command_gate = IOCommandGate::commandGate(this);
    if (!command_gate || (work_loop->addEventSource(command_gate) != kIOReturnSuccess)) {
        IOLog("%s Could not open command gate\n", getName());
        goto exit;
    }
    
    name = OSDynamicCast(OSData, getProperty("name"));
    if (name != NULL) {
        setProperty("IOName", reinterpret_cast<const char*>(name->getBytesNoCopy()));
    }
    else {
        IOLog("%s Could not load name from property!\n", getName());
        goto exit;
    }

    registerService();

    setProperty("VoodooI2CServices Supported", kOSBooleanTrue);

    return true;
exit:
    releaseResources();
    return false;
}

void VoodooI2CDeviceNub::stop(IOService* provider) {
    releaseResources();
    super::stop(provider);
}

IOReturn VoodooI2CDeviceNub::unregisterInterrupt(int source) {
    if (has_gpio_interrupts) {
        return gpio_controller->unregisterInterrupt(gpio_pin);
    } else {
        return acpi_device->unregisterInterrupt(source);
    }
}

IOReturn VoodooI2CDeviceNub::writeI2C(UInt8 *values, UInt16 length) {
    return command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &VoodooI2CDeviceNub::writeI2CGated), values, &length);
}

IOReturn VoodooI2CDeviceNub::writeI2CGated(UInt8* values, UInt16* length) {
    UInt16 flags = 0;
    if (use_10bit_addressing)
        flags = I2C_M_TEN;

    VoodooI2CControllerBusMessage msgs[] = {
        {
            .address = i2c_address,
            .buffer = values,
            .flags = flags,
            .length = *length,
        },
    };
    return controller->transferI2C(msgs, 1);
}

IOReturn VoodooI2CDeviceNub::writeReadI2C(UInt8 *write_buffer, UInt16 write_length, UInt8 *read_buffer, UInt16 read_length) {
    return command_gate->runAction(OSMemberFunctionCast(IOCommandGate::Action, this, &VoodooI2CDeviceNub::writeReadI2CGated), write_buffer, &write_length, read_buffer, &read_length);
}
IOReturn VoodooI2CDeviceNub::writeReadI2CGated(UInt8* write_buffer, UInt16* write_length, UInt8* read_buffer, UInt16* read_length) {
    UInt16 read_flags = I2C_M_RD;
    if (use_10bit_addressing)
        read_flags |= I2C_M_TEN;

    UInt16 write_flags = 0;

    if (use_10bit_addressing)
        write_flags = I2C_M_TEN;
    VoodooI2CControllerBusMessage msgs[] = {
        {
            .address = i2c_address,
            .buffer = write_buffer,
            .flags = write_flags,
            .length = *write_length,
        },
        {
            .address = i2c_address,
            .buffer = read_buffer,
            .flags = read_flags,
            .length = *read_length,
        }
    };
    return controller->transferI2C(msgs, 2);
}
