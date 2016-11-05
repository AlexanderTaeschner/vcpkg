#include "vcpkg.h"
#include <iostream>
#include <iomanip>
#include <fstream>
#include <functional>
#include <string>
#include <unordered_map>
#include <memory>
#include <filesystem>
#include <vector>
#include <cassert>
#include "vcpkg_Files.h"
#include "vcpkg_System.h"

using namespace vcpkg;

bool vcpkg::g_do_dry_run = false;

namespace
{
    template <class M, class K, class V>
    auto find_or_default(const M& map, const K& key, const V& val)
    {
        auto it = map.find(key);
        if (it == map.end())
            return decltype(it->second)(val);
        else
            return it->second;
    }
}

namespace
{
    std::fstream open_status_file(const vcpkg_paths& paths, std::ios_base::openmode mode = std::ios_base::app | std::ios_base::in | std::ios_base::out | std::ios_base::binary)
    {
        return std::fstream(paths.vcpkg_dir_status_file, mode);
    }
}

static StatusParagraphs load_current_database(const fs::path& vcpkg_dir_status_file, const fs::path& vcpkg_dir_status_file_old)
{
    if (!fs::exists(vcpkg_dir_status_file))
    {
        if (!fs::exists(vcpkg_dir_status_file_old))
        {
            // no status file, use empty db
            return StatusParagraphs();
        }

        fs::rename(vcpkg_dir_status_file_old, vcpkg_dir_status_file);
    }

    auto text = Files::get_contents(vcpkg_dir_status_file).get_or_throw();
    auto pghs = parse_paragraphs(text);

    std::vector<std::unique_ptr<StatusParagraph>> status_pghs;
    for (auto&& p : pghs)
    {
        status_pghs.push_back(std::make_unique<StatusParagraph>(p));
    }

    return StatusParagraphs(std::move(status_pghs));
}

StatusParagraphs vcpkg::database_load_check(const vcpkg_paths& paths)
{
    auto updates_dir = paths.vcpkg_dir_updates;

    std::error_code ec;
    fs::create_directory(paths.installed, ec);
    fs::create_directory(paths.vcpkg_dir, ec);
    fs::create_directory(paths.vcpkg_dir_info, ec);
    fs::create_directory(updates_dir, ec);

    const fs::path& status_file = paths.vcpkg_dir_status_file;
    const fs::path status_file_old = status_file.parent_path() / "status-old";
    const fs::path status_file_new = status_file.parent_path() / "status-new";

    StatusParagraphs current_status_db = load_current_database(status_file, status_file_old);

    auto b = fs::directory_iterator(updates_dir);
    auto e = fs::directory_iterator();
    if (b == e)
    {
        // updates directory is empty, control file is up-to-date.
        return current_status_db;
    }

    for (; b != e; ++b)
    {
        if (!fs::is_regular_file(b->status()))
            continue;
        if (b->path().filename() == "incomplete")
            continue;

        auto text = Files::get_contents(b->path()).get_or_throw();
        auto pghs = parse_paragraphs(text);
        for (auto&& p : pghs)
        {
            current_status_db.insert(std::make_unique<StatusParagraph>(p));
        }
    }

    std::fstream(status_file_new, std::ios_base::out | std::ios_base::binary | std::ios_base::trunc) << current_status_db;

    if (fs::exists(status_file_old))
        fs::remove(status_file_old);
    if (fs::exists(status_file))
        fs::rename(status_file, status_file_old);
    fs::rename(status_file_new, status_file);
    fs::remove(status_file_old);

    b = fs::directory_iterator(updates_dir);
    for (; b != e; ++b)
    {
        if (!fs::is_regular_file(b->status()))
            continue;
        fs::remove(b->path());
    }

    return current_status_db;
}

static fs::path listfile_path(const vcpkg_paths& paths, const BinaryParagraph& pgh)
{
    return paths.vcpkg_dir_info / (pgh.fullstem() + ".list");
}

static std::string get_fullpkgname_from_listfile(const fs::path& path)
{
    auto ret = path.stem().generic_u8string();
    std::replace(ret.begin(), ret.end(), '_', ':');
    return ret;
}

static void write_update(const vcpkg_paths& paths, const StatusParagraph& p)
{
    static int update_id = 0;
    auto my_update_id = update_id++;
    auto tmp_update_filename = paths.vcpkg_dir_updates / "incomplete";
    auto update_filename = paths.vcpkg_dir_updates / std::to_string(my_update_id);
    std::fstream fs(tmp_update_filename, std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);
    fs << p;
    fs.close();
    fs::rename(tmp_update_filename, update_filename);
}

static void install_and_write_listfile(const vcpkg_paths& paths, const BinaryParagraph& bpgh)
{
    std::fstream listfile(listfile_path(paths, bpgh), std::ios_base::out | std::ios_base::binary | std::ios_base::trunc);

    auto package_prefix_path = paths.package_dir(bpgh.spec);
    auto prefix_length = package_prefix_path.native().size();

    const triplet& target_triplet = bpgh.spec.target_triplet();
    const std::string& target_triplet_as_string = target_triplet.canonical_name();
    std::error_code ec;
    fs::create_directory(paths.installed / target_triplet_as_string, ec);
    listfile << target_triplet << "\n";

    for (auto it = fs::recursive_directory_iterator(package_prefix_path); it != fs::recursive_directory_iterator(); ++it)
    {
        const auto& filename = it->path().filename();
        if (fs::is_regular_file(it->status()) && (filename == "CONTROL" || filename == "control"))
        {
            // Do not copy the control file
            continue;
        }

        auto suffix = it->path().generic_u8string().substr(prefix_length + 1);
        auto target = paths.installed / target_triplet_as_string / suffix;

        auto status = it->status(ec);
        if (ec)
        {
            System::println(System::color::error, "failed: %s: %s", it->path().u8string(), ec.message());
            continue;
        }
        if (fs::is_directory(status))
        {
            fs::create_directory(target, ec);
            if (ec)
            {
                System::println(System::color::error, "failed: %s: %s", target.u8string(), ec.message());
            }

            listfile << target_triplet << "/" << suffix << "\n";
        }
        else if (fs::is_regular_file(status))
        {
            fs::copy_file(*it, target, ec);
            if (ec)
            {
                System::println(System::color::error, "failed: %s: %s", target.u8string(), ec.message());
            }
            listfile << target_triplet << "/" << suffix << "\n";
        }
        else if (!fs::status_known(status))
        {
            System::println(System::color::error, "failed: %s: unknown status", it->path().u8string());
        }
        else
            System::println(System::color::error, "failed: %s: cannot handle file type", it->path().u8string());
    }

    listfile.close();
}

// TODO: Refactoring between this function and install_package
std::vector<std::string> vcpkg::get_unmet_package_dependencies(const vcpkg_paths& paths, const package_spec& spec, const StatusParagraphs& status_db)
{
    const fs::path packages_dir_control_file_path = paths.package_dir(spec) / "CONTROL";

    auto control_contents_maybe = Files::get_contents(packages_dir_control_file_path);
    if (auto control_contents = control_contents_maybe.get())
    {
        std::vector<std::unordered_map<std::string, std::string>> pghs;
        try
        {
            pghs = parse_paragraphs(*control_contents);
        }
        catch (std::runtime_error)
        {
        }
        Checks::check_exit(pghs.size() == 1, "Invalid control file at %s", packages_dir_control_file_path.string());

        std::vector<std::string> unversioned_deps;
        for (auto&& dep : BinaryParagraph(pghs[0]).depends)
        {
            unversioned_deps.push_back(dep.name);
        }
        return unversioned_deps;
    }

    return get_unmet_package_build_dependencies(paths, spec, status_db);
}

std::vector<std::string> vcpkg::get_unmet_package_build_dependencies(const vcpkg_paths& paths, const package_spec& spec, const StatusParagraphs& status_db)
{
    const fs::path ports_dir_control_file_path = paths.port_dir(spec) / "CONTROL";
    auto control_contents_maybe = Files::get_contents(ports_dir_control_file_path);
    if (auto control_contents = control_contents_maybe.get())
    {
        std::vector<std::unordered_map<std::string, std::string>> pghs;
        try
        {
            pghs = parse_paragraphs(*control_contents);
        }
        catch (std::runtime_error)
        {
        }
        Checks::check_exit(pghs.size() == 1, "Invalid control file at %s", ports_dir_control_file_path.string());
        return filter_dependencies(SourceParagraph(pghs[0]).depends, spec.target_triplet());
    }

    Checks::exit_with_message("Could not find package named %s", spec);
}

void vcpkg::install_package(const vcpkg_paths& paths, const BinaryParagraph& binary_paragraph, StatusParagraphs& status_db)
{
    StatusParagraph spgh;
    spgh.package = binary_paragraph;
    spgh.want = want_t::install;
    spgh.state = install_state_t::half_installed;
    for (auto&& dep : spgh.package.depends)
    {
        if (status_db.find_installed(dep.name, spgh.package.spec.target_triplet()) == status_db.end())
        {
            Checks::unreachable();
        }
    }
    write_update(paths, spgh);
    status_db.insert(std::make_unique<StatusParagraph>(spgh));

    install_and_write_listfile(paths, spgh.package);

    spgh.state = install_state_t::installed;
    write_update(paths, spgh);
    status_db.insert(std::make_unique<StatusParagraph>(spgh));
}

enum class deinstall_plan
{
    not_installed,
    dependencies_not_satisfied,
    should_deinstall
};

static deinstall_plan deinstall_package_plan(
    const StatusParagraphs::iterator package_it,
    const StatusParagraphs& status_db,
    std::vector<const StatusParagraph*>& dependencies_out)
{
    dependencies_out.clear();

    if (package_it == status_db.end() || (*package_it)->state == install_state_t::not_installed)
    {
        return deinstall_plan::not_installed;
    }

    auto& pkg = (*package_it)->package;

    for (auto&& inst_pkg : status_db)
    {
        if (inst_pkg->want != want_t::install)
            continue;
        if (inst_pkg->package.spec.target_triplet() != pkg.spec.target_triplet())
            continue;

        const auto& deps = inst_pkg->package.depends;

        if (std::find_if(deps.begin(), deps.end(), [&](const auto& dep) { return dep.name == pkg.spec.name(); }) != deps.end())
        {
            dependencies_out.push_back(inst_pkg.get());
        }
    }

    if (!dependencies_out.empty())
        return deinstall_plan::dependencies_not_satisfied;

    return deinstall_plan::should_deinstall;
}

void vcpkg::deinstall_package(const vcpkg_paths& paths, const package_spec& spec, StatusParagraphs& status_db)
{
    auto package_it = status_db.find(spec.name(), spec.target_triplet());
    if (package_it == status_db.end())
    {
        System::println(System::color::success, "Package %s is not installed", spec);
        return;
    }

    auto& pkg = **package_it;

    std::vector<const StatusParagraph*> deps;
    auto plan = deinstall_package_plan(package_it, status_db, deps);
    switch (plan)
    {
        case deinstall_plan::not_installed:
            System::println(System::color::success, "Package %s is not installed", spec);
            return;
        case deinstall_plan::dependencies_not_satisfied:
            System::println(System::color::error, "Error: Cannot remove package %s:", spec);
            for (auto&& dep : deps)
            {
                System::println("  %s depends on %s", dep->package.displayname(), pkg.package.displayname());
            }
            exit(EXIT_FAILURE);
        case deinstall_plan::should_deinstall:
            break;
        default:
            Checks::unreachable();
    }

    pkg.want = want_t::purge;
    pkg.state = install_state_t::half_installed;
    write_update(paths, pkg);

    std::fstream listfile(listfile_path(paths, pkg.package), std::ios_base::in | std::ios_base::binary);
    if (listfile)
    {
        std::vector<fs::path> dirs_touched;
        std::string suffix;
        while (std::getline(listfile, suffix))
        {
            if (!suffix.empty() && suffix.back() == '\r')
                suffix.pop_back();

            std::error_code ec;

            auto target = paths.installed / suffix;

            auto status = fs::status(target, ec);
            if (ec)
            {
                System::println(System::color::error, "failed: %s", ec.message());
                continue;
            }

            if (fs::is_directory(status))
            {
                dirs_touched.push_back(target);
            }
            else if (fs::is_regular_file(status))
            {
                fs::remove(target, ec);
                if (ec)
                {
                    System::println(System::color::error, "failed: %s: %s", target.u8string(), ec.message());
                }
            }
            else if (!fs::status_known(status))
            {
                System::println(System::color::warning, "Warning: unknown status: %s", target.u8string());
            }
            else
            {
                System::println(System::color::warning, "Warning: %s: cannot handle file type", target.u8string());
            }
        }

        auto b = dirs_touched.rbegin();
        auto e = dirs_touched.rend();
        for (; b != e; ++b)
        {
            if (fs::directory_iterator(*b) == fs::directory_iterator())
            {
                std::error_code ec;
                fs::remove(*b, ec);
                if (ec)
                {
                    System::println(System::color::error, "failed: %s", ec.message());
                }
            }
        }

        listfile.close();
        fs::remove(listfile_path(paths, pkg.package));
    }

    pkg.state = install_state_t::not_installed;
    write_update(paths, pkg);
    System::println(System::color::success, "Package %s was successfully removed", pkg.package.displayname());
}

void vcpkg::search_file(const vcpkg_paths& paths, const std::string& file_substr, const StatusParagraphs& status_db)
{
    std::string line;

    for (auto&& pgh : status_db)
    {
        if (pgh->state != install_state_t::installed)
            continue;

        std::fstream listfile(listfile_path(paths, pgh->package));
        while (std::getline(listfile, line))
        {
            if (line.empty())
            {
                continue;
            }

            if (line.find(file_substr) != std::string::npos)
            {
                System::println("%s: %s", pgh->package.displayname(), line);
            }
        }
    }
}

namespace
{
    struct Binaries
    {
        std::vector<fs::path> dlls;
        std::vector<fs::path> libs;
    };

    Binaries detect_files_in_directory_ending_with(const fs::path& path)
    {
        Files::check_is_directory(path);

        Binaries binaries;

        for (auto it = fs::recursive_directory_iterator(path); it != fs::recursive_directory_iterator(); ++it)
        {
            fs::path file = *it;
            // Skip if directory ?????
            if (file.extension() == ".dll")
            {
                binaries.dlls.push_back(file);
            }
            else if (file.extension() == ".lib")
            {
                binaries.libs.push_back(file);
            }
        }

        return binaries;
    }

    void copy_files_into_directory(const std::vector<fs::path>& files, const fs::path& destination_folder)
    {
        fs::create_directory(destination_folder);

        for (auto const& src_path : files)
        {
            fs::path dest_path = destination_folder / src_path.filename();
            fs::copy(src_path, dest_path, fs::copy_options::overwrite_existing);
        }
    }

    void place_library_files_in(const fs::path& include_directory, const fs::path& project_directory, const fs::path& destination_path)
    {
        Files::check_is_directory(include_directory);
        Files::check_is_directory(project_directory);
        Files::check_is_directory(destination_path);
        Binaries debug_binaries = detect_files_in_directory_ending_with(project_directory / "Debug");
        Binaries release_binaries = detect_files_in_directory_ending_with(project_directory / "Release");

        fs::path destination_include_directory = destination_path / "include";
        fs::copy(include_directory, destination_include_directory, fs::copy_options::recursive | fs::copy_options::overwrite_existing);

        copy_files_into_directory(release_binaries.dlls, destination_path / "bin");
        copy_files_into_directory(release_binaries.libs, destination_path / "lib");

        fs::create_directory(destination_path / "debug");
        copy_files_into_directory(debug_binaries.dlls, destination_path / "debug" / "bin");
        copy_files_into_directory(debug_binaries.libs, destination_path / "debug" / "lib");
    }
}

void vcpkg::binary_import(const vcpkg_paths& paths, const fs::path& include_directory, const fs::path& project_directory, const BinaryParagraph& control_file_data)
{
    fs::path library_destination_path = paths.package_dir(control_file_data.spec);
    fs::create_directory(library_destination_path);
    place_library_files_in(include_directory, project_directory, library_destination_path);

    fs::path control_file_path = library_destination_path / "CONTROL";
    std::ofstream(control_file_path) << control_file_data;
}
