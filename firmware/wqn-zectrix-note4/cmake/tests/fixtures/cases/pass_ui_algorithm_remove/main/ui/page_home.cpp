// M8 gate fixture: erase-remove, which is not a file removal.
#include <algorithm>
#include <vector>
namespace wqn { void RenderHomePageFixture() {
    std::vector<int> v{1, 2, 3};
    v.erase(std::remove(v.begin(), v.end(), 3), v.end());
} }
