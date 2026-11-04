#include "plugin/Product.h"

#include <cstring>

namespace psplug
{
const std::vector<Product>& allProducts()
{
    static const std::vector<Product> products {
#include "ProductTable.inc"
    };
    return products;
}

const Product& suiteProduct() noexcept { return allProducts().front(); }

const Product& productById (const char* id) noexcept
{
    for (const auto& p : allProducts())
        if (std::strcmp (p.id, id) == 0)
            return p;
    return suiteProduct();
}
} // namespace psplug
