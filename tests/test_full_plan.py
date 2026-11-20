# SPDX-License-Identifier: MPL-2.0
import unittest

from full_plan import optimize, order_runs


class FullPlanTest(unittest.TestCase):
    def test_bulk_job_chooses_a_complete_route(self):
        problem = {
            "demand": {"dust": 38},
            "held": {"plate": 4, "ingot": 5, "iron_dust": 40, "carbon": 40},
            "value": {"plate": 2, "ingot": 1, "iron_dust": .7, "carbon": .2},
            "recipes": [
                {"id": "mortar", "in": {"plate": 1}, "out": {"dust": 1}},
                {"id": "bend", "in": {"ingot": 1}, "out": {"plate": 1}, "run_cost": .0336},
                {"id": "blast", "in": {"iron_dust": 1, "carbon": 1}, "out": {"ingot": 1}, "run_cost": .25},
                {"id": "macerate", "in": {"ingot": 1}, "out": {"dust": 1}, "run_cost": .01},
            ],
        }
        result = optimize(problem)
        self.assertEqual(result["status"], "optimal")
        self.assertFalse(result["missing"])
        self.assertEqual({r["id"]: r["runs"] for r in result["recipes"]}, {"blast": 33, "macerate": 38})
        self.assertIsNotNone(order_runs(problem, result))

    def test_machine_is_built_once_for_shared_work(self):
        problem = {
            "demand": {"a": 1, "b": 1},
            "held": {"iron": 3},
            "recipes": [
                {"id": "build", "in": {"iron": 1}, "out": {"machine": 1}},
                {"id": "a", "in": {"iron": 1}, "out": {"a": 1}, "machine": "machine"},
                {"id": "b", "in": {"iron": 1}, "out": {"b": 1}, "machine": "machine"},
            ],
        }
        result = optimize(problem)
        self.assertEqual({r["id"]: r["runs"] for r in result["recipes"]}, {"build": 1, "a": 1, "b": 1})
        self.assertIsNotNone(order_runs(problem, result))

    def test_cycle_cannot_be_executed_without_a_seed(self):
        problem = {
            "demand": {"a": 1},
            "recipes": [
                {"id": "make_a", "in": {"b": 1}, "out": {"a": 1}},
                {"id": "make_b", "in": {"a": 1}, "out": {"b": 1}},
            ],
        }
        result = optimize(problem)
        self.assertTrue(result["missing"])

    def test_gathered_ore_is_counted_and_scheduled(self):
        problem = {
            "demand": {"plate": 2},
            "held": {"ore": 1},
            "gather": {"ore": 16},
            "allow_missing": False,
            "recipes": [{"id": "smelt", "in": {"ore": 1}, "out": {"plate": 1}}],
        }
        result = optimize(problem)
        self.assertEqual(result["missing"], {"ore": 1})
        self.assertIsNotNone(order_runs(problem, result))


if __name__ == "__main__":
    unittest.main()
