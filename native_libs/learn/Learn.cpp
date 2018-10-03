#include <cmath>
#include <arrow/array.h>
#include <Core/ArrowUtilities.h>
#include "SKLearn.h"
#include <variant.h>
#include <LifetimeManager.h>
#include <Analysis.h>
#include "Core/Error.h"
#include "Python/PythonInterpreter.h"

COMPILATION_UNIT_USING_NUMPY;

namespace skl = sklearn;

struct NPArrayBuilder
{
    double* data = NULL;
    size_t rows = 0, cols = 0;
    void init(size_t _rows, size_t _cols)
    {
        rows = _rows;
        cols = _cols;
        // FIXME!!! I think this can leak
        data = (double*) malloc(sizeof(double)*rows*cols);
        if(!data)
            THROW("failed to allocate memory for numpy matrix of {} rows x {} columns", _rows, _cols);
    }
    void setAt(int64_t row, int64_t col, double d)
    {
        data[row*cols+col] = d;
    }
    void setAt(int64_t row, int64_t col, int64_t i)
    {
        //if (row==0) std::cout << (double) i << std::endl;
        data[row*cols+col] = (double) i;
    }
    void setAt(int64_t row, int64_t col, std::string_view)
    {
        throw std::runtime_error("Cannot use strings with numpy array.");
    }
    void setAt(int64_t row, int64_t col, Timestamp)
    {
        throw std::runtime_error("Cannot use timestamps with numpy array.");
    }
    void setNaAt(int64_t row, int64_t col)
    {
        data[row*cols+col] = nan(" ");
    }
    pybind11::array getNPMatrix()
    {
        npy_intp dims[2] = {(long) rows, (long) cols};
        pybind11::handle h = PyArray_SimpleNewFromData(2, dims, NPY_DOUBLE, data);
        return pybind11::array{h, false};
    }
    pybind11::array_t<double> getNPArr()
    {
        npy_intp dim = rows*cols;
        pybind11::handle h = PyArray_SimpleNewFromData(1, &dim, NPY_DOUBLE, data);
        return pybind11::array{ h, false };
    }
    template<typename Iterable>
    void addColumn(int columnIndex, const Iterable &iterable)
    {
        // TODO? perhaps can relax rules and fill with nulls / ignore additional values
        if(iterable.length() != rows)
            THROW("failed to add column with index {} to numpy matrix: it has {} rows while expected {}", columnIndex, iterable.length(), rows);

        int64_t rowIndex = 0;
        iterateOverGeneric(iterable,
            [&](auto&& elem)
            {
                setAt(rowIndex, columnIndex, elem);
                rowIndex++;
            },
            [&]()
            {
                setNaAt(rowIndex, columnIndex);
                rowIndex++;
            });
    }
};

auto fromC(PyObject *obj)
{
    return pybind11::reinterpret_borrow<pybind11::object>(obj);
}

auto passToC(pybind11::object obj)
{
    return obj.release().ptr();
}

pybind11::array tableToNpMatrix(const arrow::Table& table)
{
    NPArrayBuilder builder;
    builder.init(table.num_rows(), table.num_columns());
    auto cols = getColumns(table);
    int colIndex = 0;
    for (auto& col : cols)
    {
        builder.addColumn(colIndex++, *col);
    }
    auto res = builder.getNPMatrix();
    //PyObject_Print(res, stdout, 0);
    return res;
}

pybind11::array_t<double> columnToNpArr(const arrow::Column &col)
{
    NPArrayBuilder builder;
    builder.init(col.length(), 1);
    builder.addColumn(0, col);
    auto res = builder.getNPArr();
    //PyObject_Print(res, stdout, 0);
    return res;
}

std::shared_ptr<arrow::Column> npArrayToColumn(pybind11::array_t<double> arr, std::string name)
{
    // TODO: check that this is 1-D array
    // TODO: check that array contains doubles
    auto size = arr.shape(0);
    auto data = (const double *) arr.data();

    // TODO: check that array is contiguous
    arrow::DoubleBuilder builder;
    for(size_t i = 0; i < size; ++i)
    {
        auto value = data[i];
        if(std::isnan(value))
            builder.AppendNull();
        else
            builder.Append(value);
    }
    return toColumn(finish(builder), name);
}

extern "C"
{

EXPORT void toNpArr(arrow::Table* tb, const char **outError) noexcept
{
    return TRANSLATE_EXCEPTION(outError)
    {
        tableToNpMatrix(*tb);
    };
}

EXPORT void freePyObj(PyObject* o, const char **outError) noexcept
{
    return TRANSLATE_EXCEPTION(outError)
    {
        Py_XDECREF(o);
    };
}

EXPORT PyObject* newLogisticRegression(double C, const char **outError) noexcept
{
    return TRANSLATE_EXCEPTION(outError)
    {
        return passToC(skl::newLogisticRegression(C));
    };
}

EXPORT PyObject* newLinearRegression(const char **outError) noexcept
{
    return TRANSLATE_EXCEPTION(outError)
    {
        return passToC(skl::newLinearRegression());
    };
}

EXPORT void fit(PyObject* model, arrow::Table *xs, arrow::Column *y, const char **outError) noexcept
{
    return TRANSLATE_EXCEPTION(outError)
    {
        auto xsO = tableToNpMatrix(*xs);
        auto yO = columnToNpArr(*y);
        skl::fit(pybind11::reinterpret_borrow<pybind11::object>(model), xsO, yO);
    };
}

EXPORT double score(PyObject* model, arrow::Table* xs, arrow::Column* y, const char **outError) noexcept
{
    return TRANSLATE_EXCEPTION(outError)
    {
        auto xsO = tableToNpMatrix(*xs);
        auto yO = columnToNpArr(*y);
        return skl::score(fromC(model), xsO, yO);
    };
}

EXPORT arrow::Column* predict(PyObject* model, arrow::Table* xs, const char **outError) noexcept
{
    return TRANSLATE_EXCEPTION(outError)
    {
        auto xsO = tableToNpMatrix(*xs);
        auto yO = skl::predict(fromC(model), xsO);
        //PyObject_Print(yO, stdout, 0);
        return LifetimeManager::instance().addOwnership(npArrayToColumn(yO, "Predictions"));
    };
}

EXPORT arrow::Table* confusionMatrix(arrow::Column* ytrue, arrow::Column* ypred, const char **outError) noexcept
{
    return TRANSLATE_EXCEPTION(outError)
    {
        auto c1 = columnToNpArr(*ytrue);
        auto c2 = columnToNpArr(*ypred);
        auto t1 = skl::confusion_matrix(c1, c2);
//         std::cout << "\nBEFORE\n";
//         PyObject_Print(t1, stdout, 0);
//         std::cout << "\nAFTER\n";
        return nullptr;
    };
}

EXPORT arrow::Table* oneHotEncode(arrow::Column* col, const char **outError) noexcept
{
    return TRANSLATE_EXCEPTION(outError)
    {
        std::unordered_map<std::string_view, int> valIndexes;
        int lastIndex = 0;
        iterateOver<arrow::Type::STRING>(*col,
                [&](auto&& elem)
                {
                    if (valIndexes.find(elem)==valIndexes.end()) valIndexes[elem] = lastIndex++;
                },
                []() { });
        std::vector<arrow::DoubleBuilder> builders(valIndexes.size());
        iterateOver<arrow::Type::STRING>(*col,
                [&](auto&& elem)
                {
                    int ind = valIndexes[elem];
                    for (int i = 0; i<builders.size(); i++)
                    {
                        builders[i].Append(i==ind ? 1 : 0);
                    }
                },
                [&]()
                {
                    for (int i = 0; i<builders.size(); i++)
                    {
                        builders[i].Append(0);
                    }
                });
        std::vector<PossiblyChunkedArray> arrs;
        for (auto& bldr : builders)
        {
            arrs.push_back(finish(bldr));
        }
        std::vector<std::string> names(valIndexes.size());
        for (auto& item : valIndexes)
        {
            names[item.second] = col->name()+": "+std::string(item.first);
        }
        auto t = tableFromArrays(arrs, names);
        return LifetimeManager::instance().addOwnership(std::move(t));
    };
}
}

sklearn::interpreter& sklearn::interpreter::get()
{
    static sklearn::interpreter ctx;
    return ctx;
}