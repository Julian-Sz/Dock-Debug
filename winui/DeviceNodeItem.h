#pragma once

#include "DeviceNodeItem.g.h"

namespace winrt::DockDebug::implementation
{
    struct DeviceNodeItem : DeviceNodeItemT<DeviceNodeItem>
    {
        DeviceNodeItem(hstring const& label, hstring const& glyph, hstring const& instanceId)
            : m_label(label), m_glyph(glyph), m_instanceId(instanceId)
        {
        }

        hstring Label() const { return m_label; }
        hstring Glyph() const { return m_glyph; }
        hstring InstanceId() const { return m_instanceId; }

    private:
        hstring m_label;
        hstring m_glyph;
        hstring m_instanceId;
    };
}

namespace winrt::DockDebug::factory_implementation
{
    struct DeviceNodeItem : DeviceNodeItemT<DeviceNodeItem, implementation::DeviceNodeItem>
    {
    };
}
