/* Distributed under the OSI-approved BSD 3-Clause License.  See accompanying
   file LICENSE.rst or https://cmake.org/licensing for details.  */
#pragma once

#include <map>
#include <string>

class cmCTestResourceSpec;

class cmCTestResourceAllocator
{
public:
  struct Resource
  {
    unsigned int Total;
    unsigned int Locked;

    unsigned int Free() const { return this->Total - this->Locked; }

    friend bool operator==(Resource left, Resource right)
    {
      return left.Total == right.Total && left.Locked == right.Locked;
    }

    friend bool operator!=(Resource left, Resource right)
    {
      return !(left == right);
    }
  };

  void InitializeFromResourceSpec(cmCTestResourceSpec const& spec);

  std::map<std::string, std::map<std::string, Resource>> const& GetResources()
    const;

  bool AllocateResource(std::string const& name, std::string const& id,
                        unsigned int slots);
  bool DeallocateResource(std::string const& name, std::string const& id,
                          unsigned int slots);

private:
  std::map<std::string, std::map<std::string, Resource>> Resources;
};
